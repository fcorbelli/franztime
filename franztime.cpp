/*
franztime: A "time" for Windows that also measures RAM, CPU and I/O
           Provided as-is, with no warranty whatsoever,
                    by Franco Corbelli
                 franco@francocorbelli.com
MIT License
Copyright (c) 2026 Franco Corbelli

runs a command, waits for it, then prints on stderr
  wall time, CPU (user, kernel, average and peak cores), CPU cycles
  working set, private bytes, private working set: peak and final
  page faults, I/O
CPU and I/O include the child processes (job object), the exit code is the one of the command

peaks of working set and private bytes are kept by Windows (exact, no sampling)
"final" = last sample taken while the process was surely still running (not exiting)
private working set (the "Memory" of Task Manager) exists only as a sample

colors only on a console (stderr): green the main values, yellow the warnings, red the errors

no libraries to link, no DLLs besides the ones of Windows
  g++ -O2 -Wall franztime.cpp -o franztime.exe
Windows 7 or later, private working set only on newer Windows 10/11
*/

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <intrin.h>

#define FRANZTIME_VERSION "1.3"
#define FRANZTIME_DATE	  "2026-10-21"

/// in kernel32 since Windows 7, but old headers (mingw 7.3) do not declare it; the new ones the same way
extern "C" BOOL WINAPI K32GetProcessMemoryInfo(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/// PROCESS_MEMORY_COUNTERS_EX2, written here because old headers do not have it
struct franzmem
{
	DWORD	cb;
	DWORD	PageFaultCount;
	SIZE_T	PeakWorkingSetSize;
	SIZE_T	WorkingSetSize;
	SIZE_T	QuotaPeakPagedPoolUsage;
	SIZE_T	QuotaPagedPoolUsage;
	SIZE_T	QuotaPeakNonPagedPoolUsage;
	SIZE_T	QuotaNonPagedPoolUsage;
	SIZE_T	PagefileUsage;
	SIZE_T	PeakPagefileUsage;
	SIZE_T	PrivateUsage;
	SIZE_T	PrivateWorkingSetSize;
	ULONG64 SharedCommitUsage;
};
/// the fields of PROCESS_MEMORY_COUNTERS_EX, then the two of EX2
static_assert(offsetof(franzmem, PrivateUsage) == offsetof(PROCESS_MEMORY_COUNTERS_EX, PrivateUsage), "franzmem is not PROCESS_MEMORY_COUNTERS_EX2");
static_assert(offsetof(franzmem, PrivateWorkingSetSize) == sizeof(PROCESS_MEMORY_COUNTERS_EX), "franzmem is not PROCESS_MEMORY_COUNTERS_EX2");

#define MAXPROCESSI 1024

struct processo
{
	DWORD	 pid;
	HANDLE	 h;
	bool	 principale;
	bool	 finito;	   /// seen exiting or exited: no more samples
	bool	 hafinale;	   /// at least one sample while surely running
	bool	 ciclifermi;   /// exited (not only exiting): cicli cannot grow any more
	bool	 cicliko;	   /// a read of the cycles failed: they are not complete
	uint64_t cicli;
	int64_t	 finale_us;	   /// time of the last good sample
	uint64_t ws_fin;
	uint64_t priv_fin;
	uint64_t pws_fin;
	uint64_t ws_max;	   /// sampled peaks (Windows ones are read at the end)
	uint64_t priv_max;
	uint64_t pws_max;
	char	 nome[MAX_PATH * 3];
};

static processo g_proc[MAXPROCESSI];
static int		g_nproc= 0;
static bool		g_pieno= false;	   /// more than MAXPROCESSI: the others are not followed
static HANDLE	g_job  = NULL;
static HANDLE	g_principale= NULL;
static bool		g_pws	   = false;	   /// private working set available
static int64_t	g_campioni = 0;
static int64_t	g_t0	   = 0;
static int64_t	g_freq	   = 1;

/// sums of the processes running at the same moment (sampled)
static uint64_t g_ws_tot_max  = 0;
static uint64_t g_priv_tot_max= 0;
static uint64_t g_pws_tot_max = 0;
static int		g_vivi_max	  = 0;

/// peak cores: sliding window of one second, the last samples in a ring
/// from the CPU cycles (exact) and not from the CPU times, that grow by clock ticks of 15.6 ms
/// (one tick more per thread in a second = 4 threads shown as 4.06 cores)
/// but the cycles are only of the processes found by the samples, the CPU times of the whole job:
/// both peaks are kept, at the end the one of the cycles is used only if no process is missing
/// cycles are counted at the rate of the TSC, measured during the run against QueryPerformanceCounter
#define MAXFINESTRA 8192
static int64_t	g_fin_us[MAXFINESTRA];
static uint64_t g_fin_cpu[MAXFINESTRA];
static uint64_t g_fin_cicli[MAXFINESTRA];
static uint64_t g_tsc0			= 0;
static int		g_fin_primo		= 0;
static int		g_fin_n			= 0;
static double	g_core_cicli	= -1;
static int64_t	g_core_cicli_us = 0;
static double	g_core_tempi	= -1;
static int64_t	g_core_tempi_us = 0;
static int		g_logiche		= 1;

static HANDLE g_err	   = NULL;
static bool	  g_console= false;

/// colors with the attributes of the console, as zpaqfranz: every Windows, Windows Terminal too, nothing in the text
/// only on a console: never in a redirected stderr; -nocolor or the variable NO_COLOR turn them off
static bool g_colori   = false;
static WORD g_attributi= 0;	   /// of the console at the start, back after every color

/// only on white or gray text (a console with its own colors is left alone); the background is kept
static void color_save()
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	if (!g_console || !GetConsoleScreenBufferInfo(g_err, &info))
		return;
	const WORD bianco= FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
	if ((info.wAttributes & bianco) != bianco)
		return;
	wchar_t nocolor[8];
	if (GetEnvironmentVariableW(L"NO_COLOR", nocolor, 8) > 0)
		return;
	g_attributi= info.wAttributes;
	g_colori   = true;
}

/// the text written so far is already on the screen (WriteConsoleW, no buffer): the color starts from here
static void colore(WORD i_testo)
{
	if (g_colori)
		SetConsoleTextAttribute(g_err, (g_attributi & ~0x0F) | i_testo);
}
static void color_restore()
{
	if (g_colori)
		SetConsoleTextAttribute(g_err, g_attributi);
}
static void color_red()
{
	colore(FOREGROUND_RED | FOREGROUND_INTENSITY);
}
static void color_green()
{
	colore(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
}
static void color_yellow()
{
	colore(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
}
static void color_cyan()
{
	colore(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
}

static void vscrivi(const char *i_fmt, va_list i_ap)
{
	/// a command line can be 32767 characters, up to 3 bytes each in UTF-8
	static char	   buffer[262144];
	static wchar_t wbuffer[262144];
	int			   n= vsnprintf(buffer, sizeof(buffer), i_fmt, i_ap);
	if (n <= 0)
		return;
	if (n >= (int)sizeof(buffer))
		n= sizeof(buffer) - 1;
	DWORD scritti;
	if (g_console)
	{
		int w= MultiByteToWideChar(CP_UTF8, 0, buffer, n, wbuffer, 262144);
		if (w > 0)
		{
			WriteConsoleW(g_err, wbuffer, w, &scritti, NULL);
			return;
		}
	}
	WriteFile(g_err, buffer, n, &scritti, NULL);
}

static void scrivi(const char *i_fmt, ...)
{
	va_list ap;
	va_start(ap, i_fmt);
	vscrivi(i_fmt, ap);
	va_end(ap);
}

/// a piece of text in a color (color_red...), then the colors of the console
static void scrivicolore(void (*i_colore)(), const char *i_fmt, ...)
{
	i_colore();
	va_list ap;
	va_start(ap, i_fmt);
	vscrivi(i_fmt, ap);
	va_end(ap);
	color_restore();
}

static void toutf8(const wchar_t *i_testo, char *o_buffer, int i_size)
{
	o_buffer[0]= 0;
	int n	   = WideCharToMultiByte(CP_UTF8, 0, i_testo, -1, o_buffer, i_size, NULL, NULL);
	if (n <= 0)
		o_buffer[0]= 0;
	o_buffer[i_size - 1]= 0;
}

/// 1234567 => 1.234.567, as zpaqfranz, 8 buffers in turn
static char *migliaia(uint64_t i_n)
{
	static char			buffer[8][32];
	static unsigned int quale= 0;
	char			   *b	 = buffer[(quale++) & 7];
	char			   *p	 = &b[31];
	unsigned int		i	 = 0;
	*p					   = 0;
	do
	{
		if ((i % 3 == 0) && (i != 0))
			*--p= '.';
		*--p= '0' + i_n % 10;
		i_n/= 10;
		i++;
	} while (i_n != 0);
	return p;
}

/// 1234567 => 1.18 MB, as zpaqfranz
static char *tohuman(uint64_t i_n)
{
	static char			buffer[8][32];
	static unsigned int quale	  = 0;
	char			   *b		  = buffer[(quale++) & 7];
	const char		   *unita[]	  = {" B", "KB", "MB", "GB", "TB", "PB"};
	double				valore	  = (double)i_n;
	int					i		  = 0;
	while ((valore >= 1024.0) && (i < 5))
	{
		valore/= 1024.0;
		i++;
	}
	snprintf(b, 32, "%.2f %s", valore, unita[i]);
	return b;
}

static int64_t adesso_us()
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (int64_t)((double)(c.QuadPart - g_t0) * 1000000.0 / (double)g_freq);
}

/// TSC ticks in one microsecond, from the start of the program (precise after a second)
static double tsc_us()
{
	int64_t us= adesso_us();
	if (us <= 0)
		return 0;
	return (double)(__rdtsc() - g_tsc0) / (double)us;
}

/// CPU cycles of the processes found so far (the exited ones too, their handles are open)
/// the ones of an exited process are kept: asking again at every sample costs, with many processes
static uint64_t ciclitotali()
{
	uint64_t totale= 0;
	for (int i= 0; i < g_nproc; i++)
	{
		processo &p= g_proc[i];
		if (p.h && !p.ciclifermi && !p.cicliko)
		{
			/// signaled before the read: the read is the final one
			bool	uscito= p.finito && (WaitForSingleObject(p.h, 0) == WAIT_OBJECT_0);
			ULONG64 cicli = 0;
			if (QueryProcessCycleTime(p.h, &cicli))
			{
				p.cicli		= cicli;
				p.ciclifermi= uscito;
			}
			else
				p.cicliko= true;
		}
		totale+= p.cicli;
	}
	return totale;
}

/// with the CPU times one tick more per thread can show more cores than there are
static void picco(double i_core, int64_t i_us, double &io_max, int64_t &io_us)
{
	if (i_core > g_logiche)
		i_core= g_logiche;
	if (i_core > io_max)
	{
		io_max= i_core;
		io_us = i_us;
	}
}

static double secondi(uint64_t i_100ns)
{
	return (double)i_100ns / 10000000.0;
}

static uint64_t filetime(const FILETIME &i_ft)
{
	return ((uint64_t)i_ft.dwHighDateTime << 32) | i_ft.dwLowDateTime;
}

/// Ctrl+C and Ctrl+Break go to the command; franztime stays alive to print the report
static BOOL WINAPI gestorectrl(DWORD i_tipo)
{
	return (i_tipo == CTRL_C_EVENT) || (i_tipo == CTRL_BREAK_EVENT);
}

static bool leggimemoria(HANDLE i_h, franzmem &o_m)
{
	memset(&o_m, 0, sizeof(o_m));
	if (g_pws)
		o_m.cb= sizeof(franzmem);
	else
		o_m.cb= sizeof(PROCESS_MEMORY_COUNTERS_EX);
	return K32GetProcessMemoryInfo(i_h, (PROCESS_MEMORY_COUNTERS *)&o_m, o_m.cb) != 0;
}

/// on an older Windows the larger structure is refused, or the private working set stays 0
static void provapws()
{
	franzmem m;
	memset(&m, 0, sizeof(m));
	m.cb = sizeof(m);
	g_pws= K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof(m)) && (m.PrivateWorkingSetSize > 0);
}

/// the process is not exiting: its exit code is set before the address space is freed
static bool vivo(HANDLE i_h)
{
	DWORD codice= 0;
	if (!GetExitCodeProcess(i_h, &codice))
		return false;
	if (codice != STILL_ACTIVE)
		return false;
	return WaitForSingleObject(i_h, 0) == WAIT_TIMEOUT;
}

static void aggiungiprocesso(DWORD i_pid, HANDLE i_h, bool i_principale)
{
	if (g_nproc >= MAXPROCESSI)
		return;
	processo &p= g_proc[g_nproc++];
	memset(&p, 0, sizeof(p));
	p.pid		= i_pid;
	p.h			= i_h;
	p.principale= i_principale;
	p.finito	= (i_h == NULL);
	strcpy(p.nome, "?");
	if (i_h == NULL)
		return;
	/// a long path (\\?\) can be longer than MAX_PATH: with a short buffer the call fails
	static wchar_t percorso[32768];
	DWORD		   lunghezza= 32768;
	if (QueryFullProcessImageNameW(i_h, 0, percorso, &lunghezza))
	{
		const wchar_t *nome= percorso;
		for (const wchar_t *q= percorso; *q; q++)
			if ((*q == L'\\') || (*q == L'/'))
				nome= q + 1;
		toutf8(nome, p.nome, sizeof(p.nome));
	}
}

static void nuoviprocessi()
{
	if (g_job == NULL)
		return;
	static ULONG_PTR buffer[4096 + 2];
	JOBOBJECT_BASIC_PROCESS_ID_LIST *lista= (JOBOBJECT_BASIC_PROCESS_ID_LIST *)buffer;
	memset(lista, 0, sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST));
	if (!QueryInformationJobObject(g_job, JobObjectBasicProcessIdList, lista, sizeof(buffer), NULL))
		if (GetLastError() != ERROR_MORE_DATA)
			return;
	for (DWORD i= 0; i < lista->NumberOfProcessIdsInList; i++)
	{
		DWORD pid	= (DWORD)lista->ProcessIdList[i];
		bool  noto	= false;
		for (int j= 0; j < g_nproc; j++)
			if (g_proc[j].pid == pid)
			{
				noto= true;
				break;
			}
		if (noto)
			continue;
		if (g_nproc >= MAXPROCESSI)
		{
			/// not opened (it would be opened again at every sample); its cycles are missing, the CPU times of the job are not
			g_pieno= true;
			return;
		}
		/// the handle stays open until the end: the pid cannot be reused, the peaks can be read after the exit
		HANDLE h= OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
		if (h == NULL)
			h= OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
		/// between the list and OpenProcess the process can be gone and its pid given to another one, out of the job
		BOOL neljob= TRUE;
		if (h && IsProcessInJob(h, g_job, &neljob) && !neljob)
		{
			CloseHandle(h);
			continue;
		}
		aggiungiprocesso(pid, h, false);
	}
}

/// CPU in 100 ns: of the whole job, or of the command alone
static uint64_t cputotale()
{
	if (g_job)
	{
		JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ja;
		memset(&ja, 0, sizeof(ja));
		if (QueryInformationJobObject(g_job, JobObjectBasicAccountingInformation, &ja, sizeof(ja), NULL))
			return ja.TotalUserTime.QuadPart + ja.TotalKernelTime.QuadPart;
		return 0;
	}
	FILETIME creato, uscito, kernel, utente;
	if (GetProcessTimes(g_principale, &creato, &uscito, &kernel, &utente))
		return filetime(kernel) + filetime(utente);
	return 0;
}

static void campiona(int64_t i_us, bool i_ultimo)
{
	nuoviprocessi();
	uint64_t ws	  = 0;
	uint64_t priv = 0;
	uint64_t pws  = 0;
	int		 vivi = 0;
	for (int i= 0; i < g_nproc; i++)
	{
		processo &p= g_proc[i];
		if (p.finito)
			continue;
		franzmem m;
		bool	 letto= leggimemoria(p.h, m);
		/// checked AFTER reading: if it is running now, it was running during the read
		if (!vivo(p.h))
		{
			p.finito= true;
			continue;
		}
		if (!letto)
			continue;
		p.hafinale = true;
		p.finale_us= i_us;
		p.ws_fin   = m.WorkingSetSize;
		p.priv_fin = m.PrivateUsage;
		p.pws_fin  = m.PrivateWorkingSetSize;
		if (p.ws_fin > p.ws_max)
			p.ws_max= p.ws_fin;
		if (p.priv_fin > p.priv_max)
			p.priv_max= p.priv_fin;
		if (p.pws_fin > p.pws_max)
			p.pws_max= p.pws_fin;
		ws+= p.ws_fin;
		priv+= p.priv_fin;
		pws+= p.pws_fin;
		vivi++;
	}
	if (ws > g_ws_tot_max)
		g_ws_tot_max= ws;
	if (priv > g_priv_tot_max)
		g_priv_tot_max= priv;
	if (pws > g_pws_tot_max)
		g_pws_tot_max= pws;
	if (vivi > g_vivi_max)
		g_vivi_max= vivi;
	g_campioni++;
	if (i_ultimo)
		return;

	uint64_t cpu  = cputotale();
	uint64_t cicli= ciclitotali();

	/// window from the most recent sample at least 1 s old: every window of the samples, not only one per second
	if (g_fin_n == MAXFINESTRA)
	{
		g_fin_primo= (g_fin_primo + 1) % MAXFINESTRA;
		g_fin_n--;
	}
	int ultimo		   = (g_fin_primo + g_fin_n) % MAXFINESTRA;
	g_fin_us[ultimo]   = i_us;
	g_fin_cpu[ultimo]  = cpu;
	g_fin_cicli[ultimo]= cicli;
	g_fin_n++;
	while ((g_fin_n >= 2) && (i_us - g_fin_us[(g_fin_primo + 1) % MAXFINESTRA] >= 1000000))
	{
		g_fin_primo= (g_fin_primo + 1) % MAXFINESTRA;
		g_fin_n--;
	}
	if ((g_fin_n >= 2) && (i_us - g_fin_us[g_fin_primo] >= 1000000))
	{
		int	   p  = g_fin_primo;
		double us = (double)(i_us - g_fin_us[p]);
		double tsc= tsc_us();
		picco(((double)(cpu - g_fin_cpu[p]) / 10.0) / us, i_us, g_core_tempi, g_core_tempi_us);
		if (tsc > 0)
			picco((double)(cicli - g_fin_cicli[p]) / tsc / us, i_us, g_core_cicli, g_core_cicli_us);
	}
}

/// skips argv[0] as the C runtime does: "..." or up to the first blank
static const wchar_t *saltaprimo(const wchar_t *p)
{
	if (*p == L'"')
	{
		p++;
		while (*p && (*p != L'"'))
			p++;
		if (*p)
			p++;
	}
	else
		while (*p && (*p != L' ') && (*p != L'\t'))
			p++;
	while ((*p == L' ') || (*p == L'\t'))
		p++;
	return p;
}

/// one argument, with the rules of the C runtime for quotes and backslashes
static const wchar_t *prendiarg(const wchar_t *p, wchar_t *o_arg, int i_size)
{
	int	 n		= 0;
	bool virgolette= false;
	while (*p)
	{
		if (!virgolette && ((*p == L' ') || (*p == L'\t')))
			break;
		if (*p == L'\\')
		{
			int barre= 0;
			while (*p == L'\\')
			{
				barre++;
				p++;
			}
			if (*p == L'"')
			{
				for (int i= 0; i < barre / 2; i++)
					if (n < i_size - 1)
						o_arg[n++]= L'\\';
				if (barre & 1)
				{
					if (n < i_size - 1)
						o_arg[n++]= L'"';
					p++;
				}
			}
			else
				for (int i= 0; i < barre; i++)
					if (n < i_size - 1)
						o_arg[n++]= L'\\';
			continue;
		}
		if (*p == L'"')
		{
			if (virgolette && (p[1] == L'"'))
			{
				if (n < i_size - 1)
					o_arg[n++]= L'"';
				p+= 2;
				continue;
			}
			virgolette= !virgolette;
			p++;
			continue;
		}
		if (n < i_size - 1)
			o_arg[n++]= *p;
		p++;
	}
	o_arg[n]= 0;
	while ((*p == L' ') || (*p == L'\t'))
		p++;
	return p;
}

static void aiuto()
{
	scrivicolore(color_green, "franztime v" FRANZTIME_VERSION ",(" FRANZTIME_DATE ")\n");
	scrivi("runs a command,then shows time,CPU,RAM ");
	scrivicolore(color_cyan,"(C) 2026 by Franco Corbelli\n");
	scrivi("\n");
	scrivi("Usage: ");
	scrivicolore(color_cyan,"franztime ");
	scrivi("[options] program [arguments...]\n");
	scrivicolore(color_green,"  -i N         ");
	scrivi("sampling interval in ms (default 10)\n");
	scrivicolore(color_green,"  -nocolor     ");
	scrivi("no colors (also with the environment variable NO_COLOR)\n");
	scrivicolore(color_green,"  -nocolor     ");
	scrivi("no colors (also with the environment variable NO_COLOR)\n");
	scrivicolore(color_green,"  --           ");
	scrivi("end of the options (for a program whose name starts with -)\n");
	scrivi("\n");
	scrivi("Working set    memory of the program in RAM, private and shared (DLLs, mapped files)\n");
	scrivi("Private bytes  private memory committed or in the pagefile or never touched: not the RAM used\n");
	scrivi("Private WS     sampled private memory in RAM (private working set, the \"Memory\" of Task Manager)\n");
	scrivi("Final values = last sample taken while the program was still running (not exiting).\n");
	scrivi("\n");
	scrivi("CPU total      user+kernel times: they grow by clock ticks, fine on a long run, rough on a short\n");
	scrivi("CPU cycles     exact (QueryProcessCycleTime), at the rate of the TSC, also in seconds\n");
	scrivi("CPU peak       highest cores used over 1 s, among the windows that start and end on a sample\n");
	scrivi("\n");
	scrivi("CPU, I/O and page faults: all the processes of a job object, if available\n");
	scrivi("With more processes: a table of the ones found by the samples (at most %d)\n", MAXPROCESSI);
	scrivi("\n");
	scrivicolore(color_cyan,"Examples:\n");
	scrivi("  franztime zpaqfranz a z:\\1.zpaq c:\\dati -m1\n");
	scrivi("  franztime cmd /c \"tar -cf - c:\\dati | zstd -1 -o z:\\1.tar.zst\"\n");
}

static void erroresistema(const char *i_cosa)
{
	DWORD	errore= GetLastError();
	wchar_t testo[1024];
	char	testo8[3072];
	testo[0]= 0;
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, errore, 0, testo, 1024, NULL);
	toutf8(testo, testo8, sizeof(testo8));
	scrivicolore(color_red, "franztime: %s (error %u) %s%s", i_cosa, (unsigned)errore, testo8, testo8[0] ? "" : "\n");
}

struct finali
{
	uint64_t ws_picco;
	uint64_t priv_picco;
	uint64_t utente;
	uint64_t kernel;
	uint64_t cicli;
	DWORD	 faults;
	bool	 letto;
};

int main()
{
	g_err= GetStdHandle(STD_ERROR_HANDLE);
	DWORD modo;
	g_console= GetConsoleMode(g_err, &modo) != 0;
	color_save();

	static wchar_t arg[32768];
	int			   intervallo_ms= 10;

	const wchar_t *p	  = saltaprimo(GetCommandLineW());
	const wchar_t *comando= NULL;
	while (*p)
	{
		const wchar_t *inizio= p;
		p					 = prendiarg(p, arg, 32768);
		if (arg[0] != L'-')
		{
			comando= inizio;
			break;
		}
		if (wcscmp(arg, L"--") == 0)
		{
			if (*p)
				comando= p;
			break;
		}
		if ((_wcsicmp(arg, L"-h") == 0) || (_wcsicmp(arg, L"-help") == 0) || (wcscmp(arg, L"-?") == 0))
		{
			aiuto();
			return 0;
		}
		if (_wcsicmp(arg, L"-nocolor") == 0)
		{
			g_colori= false;
			continue;
		}
		char arg8[1024];
		toutf8(arg, arg8, sizeof(arg8));
		if (_wcsicmp(arg, L"-i") != 0)
		{
			scrivicolore(color_red, "franztime: unknown option %s (franztime -h for help)\n", arg8);
			return 127;
		}
		if (*p == 0)
		{
			scrivicolore(color_red, "franztime: %s needs a value\n", arg8);
			return 127;
		}
		static wchar_t valore[32768];
		p			 = prendiarg(p, valore, 32768);
		intervallo_ms= _wtoi(valore);
	}
	if ((comando == NULL) || (*comando == 0))
	{
		aiuto();
		return 0;
	}
	if ((intervallo_ms < 1) || (intervallo_ms > 60000))
	{
		scrivicolore(color_red, "franztime: -i must be between 1 and 60000 ms\n");
		return 127;
	}

	static wchar_t riga[32768];
	wcsncpy(riga, comando, 32767);
	riga[32767]= 0;
	/// trailing blanks of the command line out of the report
	size_t lunghezza= wcslen(riga);
	while ((lunghezza > 0) && ((riga[lunghezza - 1] == L' ') || (riga[lunghezza - 1] == L'\t')))
		riga[--lunghezza]= 0;
	static char riga8[32768 * 3];
	toutf8(riga, riga8, sizeof(riga8));

	SetConsoleCtrlHandler(gestorectrl, TRUE);
	provapws();
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	g_freq	 = f.QuadPart;
	g_logiche= (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	if (g_logiche < 1)
		g_logiche= 1;

	g_job= CreateJobObjectW(NULL, NULL);
	if (g_job)
	{
		/// a command that asks to leave the job may do it, as without franztime (otherwise its CreateProcess fails):
		/// those children are not counted
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limiti;
		memset(&limiti, 0, sizeof(limiti));
		limiti.BasicLimitInformation.LimitFlags= JOB_OBJECT_LIMIT_BREAKAWAY_OK;
		SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limiti, sizeof(limiti));
	}

	STARTUPINFOW si;
	memset(&si, 0, sizeof(si));
	si.cb= sizeof(si);
	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));
	/// handles inherited as cmd does: redirections of the console keep working
	if (!CreateProcessW(NULL, riga, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
	{
		char cosa[32768 * 3 + 64];
		snprintf(cosa, sizeof(cosa), "cannot run %s\n          ", riga8);
		erroresistema(cosa);
		return 127;
	}
	bool senzajob= false;
	if (g_job && !AssignProcessToJobObject(g_job, pi.hProcess))
	{
		CloseHandle(g_job);
		g_job= NULL;
	}
	if (g_job == NULL)
		senzajob= true;
	g_principale= pi.hProcess;
	aggiungiprocesso(pi.dwProcessId, pi.hProcess, true);

	/// the sampling must not be late when the command takes every core
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	HANDLE timer= CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	g_t0  = t.QuadPart;
	g_tsc0= __rdtsc();
	if (ResumeThread(pi.hThread) == (DWORD)-1)
	{
		/// it would stay suspended forever, and franztime with it
		erroresistema("cannot start the program");
		TerminateProcess(pi.hProcess, 127);
		return 127;
	}
	CloseHandle(pi.hThread);

	bool attesafallita= false;
	for (;;)
	{
		campiona(adesso_us(), false);
		DWORD attesa= WAIT_FAILED;
		if (timer)
		{
			LARGE_INTEGER quando;
			quando.QuadPart= -(LONGLONG)intervallo_ms * 10000;
			if (SetWaitableTimer(timer, &quando, 0, NULL, NULL, FALSE))
			{
				HANDLE oggetti[2]= {pi.hProcess, timer};
				attesa			 = WaitForMultipleObjects(2, oggetti, FALSE, INFINITE);
			}
			if (attesa == WAIT_FAILED)
			{
				/// go on without the timer, as on an old Windows, from this same wait
				CloseHandle(timer);
				timer= NULL;
			}
		}
		if (timer == NULL)
			attesa= WaitForSingleObject(pi.hProcess, intervallo_ms);
		if (attesa == WAIT_OBJECT_0)
			break;
		if (attesa == WAIT_FAILED)
		{
			/// the handle of the program does not work: the report cannot be trusted
			erroresistema("cannot wait for the program");
			attesafallita= true;
			break;
		}
	}
	int64_t fine_us= adesso_us();
	campiona(fine_us, true);

	DWORD codice= 0;
	GetExitCodeProcess(pi.hProcess, &codice);

	/// what Windows kept: peaks, times, cycles
	static finali fin[MAXPROCESSI];
	uint64_t	  cicli_tot= 0;
	int			  coperti  = 0;	   /// processes with all their cycles
	for (int i= 0; i < g_nproc; i++)
	{
		finali &x= fin[i];
		memset(&x, 0, sizeof(x));
		if (g_proc[i].h == NULL)
			continue;
		franzmem m;
		if (leggimemoria(g_proc[i].h, m))
		{
			x.letto		= true;
			x.ws_picco	= m.PeakWorkingSetSize;
			x.priv_picco= m.PeakPagefileUsage;
			x.faults	= m.PageFaultCount;
		}
		/// a sample can see more than the peak only if Windows did not give it
		if (g_proc[i].ws_max > x.ws_picco)
			x.ws_picco= g_proc[i].ws_max;
		if (g_proc[i].priv_max > x.priv_picco)
			x.priv_picco= g_proc[i].priv_max;
		FILETIME creato, uscito, kernel, utente;
		if (GetProcessTimes(g_proc[i].h, &creato, &uscito, &kernel, &utente))
		{
			x.utente= filetime(utente);
			x.kernel= filetime(kernel);
		}
		ULONG64 cicli= 0;
		if (QueryProcessCycleTime(g_proc[i].h, &cicli))
		{
			x.cicli= cicli;
			if (!g_proc[i].cicliko)
				coperti++;
		}
		cicli_tot+= x.cicli;
	}

	uint64_t utente= fin[0].utente;
	uint64_t kernel= fin[0].kernel;
	uint64_t pagefault= fin[0].faults;
	uint64_t letti_b= 0, scritti_b= 0, altri_b= 0;
	uint64_t letti_n= 0, scritti_n= 0, altri_n= 0;
	uint64_t commit_job= 0;
	int		 processi  = 1;
	int		 attivi	   = 0;
	if (g_job)
	{
		JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION ja;
		memset(&ja, 0, sizeof(ja));
		if (QueryInformationJobObject(g_job, JobObjectBasicAndIoAccountingInformation, &ja, sizeof(ja), NULL))
		{
			utente	 = ja.BasicInfo.TotalUserTime.QuadPart;
			kernel	 = ja.BasicInfo.TotalKernelTime.QuadPart;
			pagefault= ja.BasicInfo.TotalPageFaultCount;
			processi = ja.BasicInfo.TotalProcesses;
			attivi	 = ja.BasicInfo.ActiveProcesses;
			letti_b	 = ja.IoInfo.ReadTransferCount;
			scritti_b= ja.IoInfo.WriteTransferCount;
			altri_b	 = ja.IoInfo.OtherTransferCount;
			letti_n	 = ja.IoInfo.ReadOperationCount;
			scritti_n= ja.IoInfo.WriteOperationCount;
			altri_n	 = ja.IoInfo.OtherOperationCount;
		}
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl;
		memset(&jl, 0, sizeof(jl));
		if (QueryInformationJobObject(g_job, JobObjectExtendedLimitInformation, &jl, sizeof(jl), NULL))
			commit_job= jl.PeakJobMemoryUsed;
	}
	else
	{
		IO_COUNTERS io;
		if (GetProcessIoCounters(pi.hProcess, &io))
		{
			letti_b	 = io.ReadTransferCount;
			scritti_b= io.WriteTransferCount;
			altri_b	 = io.OtherTransferCount;
			letti_n	 = io.ReadOperationCount;
			scritti_n= io.WriteOperationCount;
			altri_n	 = io.OtherOperationCount;
		}
	}
	double	 tempo		= (double)fine_us / 1000000.0;
	uint64_t cpu		= utente + kernel;
	double	 core		= (tempo > 0) ? secondi(cpu) / tempo : 0;
	/// the cycles are complete only if every process of the job was found, opened and read
	bool	 cicliinteri= (coperti == g_nproc) && (coperti == processi);
	bool	 dacicli	= cicliinteri && (g_core_cicli >= 0);
	double	 core_picco = dacicli ? g_core_cicli : g_core_tempi;
	int64_t	 picco_us	= dacicli ? g_core_cicli_us : g_core_tempi_us;
	if (core_picco < 0)	   /// less than one second: no window
		core_picco= core;
	bool	  multi= (processi > 1) || (g_nproc > 1);
	processo &pp   = g_proc[0];
	finali	 &fp   = fin[0];

	const char *l= "%-20s ";
	color_restore();	/// the program may have left a color of its own
	scrivi("\n");
	scrivicolore(color_cyan, "franztime v" FRANZTIME_VERSION "  %s\n", riga8);
	scrivi(l, "Exit code");
	scrivicolore((codice == 0) ? color_green : color_red, "%d\n", (int)codice);
	scrivi(l, "Wall time");
	scrivicolore(color_green, "%.3f s\n", tempo);
	scrivi(l, "CPU total");
	scrivicolore(color_green, "%.3f s", secondi(cpu));
	scrivi(" (user %.3f s, kernel %.3f s)\n", secondi(utente), secondi(kernel));
	scrivi(l, "CPU average");
	scrivi("%.2f cores (%.1f%% of %d logical CPUs)\n", core, core * 100.0 / g_logiche, g_logiche);
	scrivi(l, "CPU peak");
	if (g_core_tempi >= 0)
		scrivi("%.2f cores (best 1 s between samples, ending at %.1f s%s)\n", core_picco, picco_us / 1000000.0, dacicli ? "" : ", from the CPU times");
	else
		scrivi("%.2f cores (run shorter than 1 s: the average)\n", core_picco);
	scrivi(l, "CPU cycles");
	scrivi("%s", migliaia(cicli_tot));
	double tsc= tsc_us();
	if (tsc > 0)
		scrivi(" (%.3f s at the %.2f GHz of the TSC)", (double)cicli_tot / tsc / 1000000.0, tsc / 1000.0);
	if (!cicliinteri)
		scrivicolore(color_yellow, ", only %d of %d processes", coperti, processi);
	scrivi("\n");
	if (multi)
		scrivicolore(color_cyan, "--- %s (pid %u), the program itself\n", pp.nome, (unsigned)pp.pid);
	scrivi(l, "Working set peak");
	scrivicolore(color_green, "%18s  %10s", migliaia(fp.ws_picco), tohuman(fp.ws_picco));
	scrivi("  (Windows)\n");
	scrivi(l, "Working set final");
	if (pp.hafinale)
		scrivi("%18s  %10s\n", migliaia(pp.ws_fin), tohuman(pp.ws_fin));
	else
		scrivi("%18s\n", "n/a");
	scrivi(l, "Private bytes peak");
	scrivicolore(color_green, "%18s  %10s", migliaia(fp.priv_picco), tohuman(fp.priv_picco));
	scrivi("  (Windows, commit)\n");
	scrivi(l, "Private bytes final");
	if (pp.hafinale)
		scrivi("%18s  %10s\n", migliaia(pp.priv_fin), tohuman(pp.priv_fin));
	else
		scrivi("%18s\n", "n/a");
	if (g_pws)
	{
		scrivi(l, "Private WS peak");
		scrivi("%18s  %10s  (sampled)\n", migliaia(pp.pws_max), tohuman(pp.pws_max));
		scrivi(l, "Private WS final");
		if (pp.hafinale)
			scrivi("%18s  %10s\n", migliaia(pp.pws_fin), tohuman(pp.pws_fin));
		else
			scrivi("%18s\n", "n/a");
	}
	else
	{
		scrivi(l, "Private WS");
		scrivi("%18s  (needs a newer Windows)\n", "n/a");
	}
	scrivi(l, "Final values");
	if (pp.hafinale)
		scrivi("last sample %.3f s before the end\n", (fine_us - pp.finale_us) / 1000000.0);
	else
		scrivi("no sample while running (too short)\n");
	if (multi)
	{
		scrivicolore(color_cyan, "--- all the processes\n");
		scrivi(l, "Processes");
		scrivi("%d (%d found by the samples, at most %d at the same time)", processi, g_nproc, g_vivi_max);
		if (attivi > 0)
			scrivicolore(color_yellow, ", %d still running", attivi);
		if (g_pieno)
			scrivicolore(color_yellow, ", table full");
		scrivi("\n");
		scrivicolore(color_cyan, "  %8s %-24s %10s %11s %11s %11s %11s\n", "pid", "name", "CPU s", "WS peak", "priv peak", "WS final", "priv final");
		for (int i= 0; i < g_nproc; i++)
		{
			processo &x= g_proc[i];
			if (x.h == NULL)
			{
				scrivi("  %8u %-24s (cannot open)\n", (unsigned)x.pid, x.nome);
				continue;
			}
			char wsf[32]   = "n/a";
			char privf[32] = "n/a";
			if (x.hafinale)
			{
				snprintf(wsf, sizeof(wsf), "%s", tohuman(x.ws_fin));
				snprintf(privf, sizeof(privf), "%s", tohuman(x.priv_fin));
			}
			scrivi("  %8u %-24.24s %10.3f %11s %11s %11s %11s\n", (unsigned)x.pid, x.nome, secondi(fin[i].utente + fin[i].kernel), tohuman(fin[i].ws_picco), tohuman(fin[i].priv_picco), wsf, privf);
		}
		scrivi(l, "Working set peak");
		scrivicolore(color_green, "%18s  %10s", migliaia(g_ws_tot_max), tohuman(g_ws_tot_max));
		scrivi("  (sum in one sample)\n");
		scrivi(l, "Private bytes peak");
		scrivicolore(color_green, "%18s  %10s", migliaia(g_priv_tot_max), tohuman(g_priv_tot_max));
		scrivi("  (sum in one sample)\n");
		if (g_pws)
		{
			scrivi(l, "Private WS peak");
			scrivi("%18s  %10s  (sum in one sample)\n", migliaia(g_pws_tot_max), tohuman(g_pws_tot_max));
		}
		if (g_job)
		{
			scrivi(l, "Job commit peak");
			scrivi("%18s  %10s  (Windows, counter of the job)\n", migliaia(commit_job), tohuman(commit_job));
		}
	}
	scrivi(l, "Page faults");
	scrivi("%s\n", migliaia(pagefault));
	scrivi(l, "I/O read");
	scrivi("%18s  %10s  (%s ops)\n", migliaia(letti_b), tohuman(letti_b), migliaia(letti_n));
	scrivi(l, "I/O write");
	scrivi("%18s  %10s  (%s ops)\n", migliaia(scritti_b), tohuman(scritti_b), migliaia(scritti_n));
	scrivi(l, "I/O other");
	scrivi("%18s  %10s  (%s ops)\n", migliaia(altri_b), tohuman(altri_b), migliaia(altri_n));
	scrivi(l, "Samples");
	scrivi("%s every %d ms%s\n", migliaia(g_campioni), intervallo_ms, timer ? "" : " (timer of the system)");
	if (senzajob)
		scrivicolore(color_yellow, "*** no job object: CPU and I/O of the program only, children not counted\n");
	if (g_pieno)
		scrivicolore(color_yellow, "*** more than %d processes: the others are not in the table, in the sums of memory, in the cycles\n", MAXPROCESSI);
	if (attesafallita)
		scrivicolore(color_red, "*** the wait for the program failed: the program may be still running, the report is not complete\n");

	for (int i= 0; i < g_nproc; i++)
		if (g_proc[i].h)
			CloseHandle(g_proc[i].h);
	if (timer)
		CloseHandle(timer);
	if (g_job)
		CloseHandle(g_job);
	return (int)codice;
}

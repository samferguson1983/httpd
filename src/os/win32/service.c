/* ====================================================================
 * Copyright (c) 1995-2000 The Apache Group.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://httpd.apache.org/)."
 *
 * 4. The names "Apache Server" and "Apache Group" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache"
 *    nor may "Apache" appear in their names without prior written
 *    permission of the Apache Group.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://httpd.apache.org/)."
 *
 * THIS SOFTWARE IS PROVIDED BY THE APACHE GROUP ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE APACHE GROUP OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Group and was originally based
 * on public domain software written at the National Center for
 * Supercomputing Applications, University of Illinois, Urbana-Champaign.
 * For more information on the Apache Group and the Apache HTTP server
 * project, please see <http://www.apache.org/>.
 *
 */

#ifdef WIN32

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <process.h>
#include <direct.h>

#include "httpd.h"
#include "http_conf_globals.h"
#include "http_log.h"
#include "http_main.h"
#include "multithread.h"
#include "service.h"
#include "registry.h"

#define SERVICE_APACHE_RESTART 128

static struct
{
    int (*main_fn)(int, char **);
    event *stop_event;
    int connected;
    SERVICE_STATUS_HANDLE hServiceStatus;
    char *name;
    int exit_status;
    SERVICE_STATUS ssStatus;
    FILE *logFile;
} globdat;

/* statics for atexit processing or shared between threads */
static BOOL  die_on_logoff = FALSE;
static DWORD monitor_thread_id = 0;
static DWORD (WINAPI *RegisterServiceProcess)(DWORD, DWORD);
static HINSTANCE monitor_hkernel = NULL;
static DWORD dos_child_procid = 0;
static HHOOK catch_term_hook = NULL;
static HWND  console_wnd = NULL;
static int   is_service = -1;


static void WINAPI service_main_fn(DWORD, LPTSTR *);
static void WINAPI service_ctrl(DWORD ctrlCode);
static int ReportStatusToSCMgr(int currentState, int exitCode, int waitHint);
static int ap_start_service(SC_HANDLE);
static int ap_stop_service(SC_HANDLE);
static int ap_restart_service(SC_HANDLE);

/* exit() for Win32 is macro mapped (horrible, we agree) that allows us 
 * to catch the non-zero conditions and inform the console process that
 * the application died, and hang on to the console a bit longer.
 *
 * The macro only maps for http_main.c and other sources that include
 * the service.h header, so we best assume it's an error to exit from
 * _any_ other module.
 *
 * If real_exit_code is reset to 0, it will not be set or trigger this
 * behavior on exit.  All service and child processes are expected to
 * reset this flag to zero to avoid undesireable side effects.
 */
int real_exit_code = 1;

void hold_console_open_on_error(void)
{
    HANDLE hConIn;
    HANDLE hConErr;
    DWORD result;
    DWORD mode;
    time_t start;
    time_t remains;
    char *msg = "Note the errors or messages above, "
                "and press the <ESC> key to exit.  ";
    CONSOLE_SCREEN_BUFFER_INFO coninfo;
    INPUT_RECORD in;
    char count[16];
    
    if (!real_exit_code)
        return;
    hConIn = GetStdHandle(STD_INPUT_HANDLE);
    hConErr = GetStdHandle(STD_ERROR_HANDLE);
    if ((hConIn == INVALID_HANDLE_VALUE) || (hConErr == INVALID_HANDLE_VALUE))
        return;
    if (!WriteConsole(hConErr, msg, strlen(msg), &result, NULL) || !result)
        return;
    if (!GetConsoleScreenBufferInfo(hConErr, &coninfo))
        return;
    if (isWindowsNT())
        mode = ENABLE_MOUSE_INPUT | 0x80;
    else
        mode = ENABLE_MOUSE_INPUT;
    if (!SetConsoleMode(hConIn, mode))
        return;
        
    start = time(NULL);
    do
    {
        while (PeekConsoleInput(hConIn, &in, 1, &result) && result)
        {
            if (!ReadConsoleInput(hConIn, &in, 1, &result) || !result)
                return;
            if ((in.EventType == KEY_EVENT) && in.Event.KeyEvent.bKeyDown 
                    && (in.Event.KeyEvent.uChar.AsciiChar == 27))
                return;
            if (in.EventType == MOUSE_EVENT 
                    && (in.Event.MouseEvent.dwEventFlags == DOUBLE_CLICK))
                return;
        }
        remains = ((start + 30) - time(NULL)); 
        sprintf (count, "%d...", remains);
        if (!SetConsoleCursorPosition(hConErr, coninfo.dwCursorPosition))
            return;
        if (!WriteConsole(hConErr, count, strlen(count), &result, NULL) 
                || !result)
            return;
    }
    while ((remains > 0) && WaitForSingleObject(hConIn, 1000) != WAIT_FAILED);
}

/* Console Control handler for processing Ctrl-C/Ctrl-Break and
 * on Windows NT also user logoff and system shutdown,
 * this is not used for the WinNT or Win9x hidden service processes
 */
static BOOL CALLBACK ap_control_handler(DWORD ctrl_type)
{
    switch (ctrl_type)
    {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            real_exit_code = 0;
            fprintf(stderr, "Apache server interrupted...\n");
            /* for Interrupt signals, shut down the server.
             * Tell the system we have dealt with the signal
             * without waiting for Apache to terminate.
             */
            ap_start_shutdown();
            return TRUE;

        case CTRL_LOGOFF_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            /* for Terminate signals, shut down the server.
             * Wait for Apache to terminate, but respond
             * after a reasonable time to tell the system
             * that we have already tried to shut down.
             */            
            real_exit_code = 0;
            fprintf(stderr, "Apache server shutdown initiated...\n");
            ap_start_shutdown();
            Sleep(30000);
            return TRUE;
    }
 
    /* We should never get here, but this is (mostly) harmless */
    return FALSE;
}

/* This is an experimental feature I'm playing with to intercept the
 * messages to the console process on Win9x.  It does nothing at all
 * so far (isn't even properly hooked yet).
 */
static LRESULT CALLBACK HookCatchTerm(int nCode, WPARAM wParam, LPARAM lParam)
{
    LPMSG msg = (LPMSG) lParam;
    if (msg->hwnd == console_wnd) {
        if (msg->message == WM_QUERYENDSESSION 
         || msg->message == WM_ENDSESSION)
            DebugBreak();
            msg->message = WM_NULL;
    }
    return CallNextHookEx(catch_term_hook, nCode, wParam, lParam);
}

/* This is the WndProc procedure for our invisible window.
 * When the user shuts down the system, this window is sent
 * a signal WM_QUERYENDSESSION with lParam == 0 to indicate
 * a system shutdown. We clean up by shutting down Apache and
 * indicate to the system that the message was received and
 * understood (return TRUE).
 * If a user logs off, the window is sent WM_QUERYENDSESSION 
 * as well, but with lParam != 0. We ignore this case.
 */
LRESULT CALLBACK Service9xWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_QUERYENDSESSION)
    {
        return (!(lParam & ENDSESSION_LOGOFF) || die_on_logoff);
    }
    if (message == WM_ENDSESSION)
    {
        /* Hmmm... not logging out, must be shutting down */
        if (dos_child_procid) {
            if (!(lParam & ENDSESSION_LOGOFF)) {
                /* Release the winoldap window from the child so that it
                 * won't interfere with the shutdown
                 */
                RegisterServiceProcess(dos_child_procid, 0);
                dos_child_procid = 0;
                FreeConsole();
            }
        }
        else if (!(lParam & ENDSESSION_LOGOFF) || die_on_logoff)
        {
            /* Tell Apache to shut down gracefully and sleep it off 
             */
            ap_start_shutdown();
   	    Sleep(60000);
        }
        return 0;
    }
    return (DefWindowProc(hWnd, message, wParam, lParam));
}

DWORD WINAPI WatchWindow(void *service_name)
{
    /* When running as a service under Windows 9x, ConsoleCtrlHandler 
     * does not respond properly when the user logs off or the system 
     * is shutdown.  If the WatchWindow thread is created with a NULL
     * service_name argument, then the ...SystemMonitor window class is
     * used to create the "Apache" window to watch for logoff and shutdown.
     * If the service_name is provided, the ...ServiceMonitor window class
     * is used to create the window named by the service_name argument,
     * and the logoff message is ignored.
     */
    WNDCLASS wc;
    HWND hwndMain;
    MSG msg;
    wc.style         = CS_GLOBALCLASS;
    wc.lpfnWndProc   = (WNDPROC) Service9xWndProc; 
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = 0; 
    wc.hInstance     = NULL;
    wc.hIcon         = NULL;
    wc.hCursor       = NULL;
    wc.hbrBackground = NULL;
    wc.lpszMenuName  = NULL;
    if (service_name)
	wc.lpszClassName = "ApacheWin95ServiceMonitor";
    else if (!dos_child_procid)
        wc.lpszClassName = "ApacheWin95ConsoleMonitor";
    else
	wc.lpszClassName = "ApacheWin95ChildMonitor";

    /* hook window messages - ugly */
//  if (dos_child_procid)
//      catch_term_hook = SetWindowsHookEx(WH_GETMESSAGE, 
//                                         HookCatchTerm, NULL, 0);

    die_on_logoff = service_name ? FALSE : TRUE;

    if (!RegisterClass(&wc)) 
    {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                     "Could not register window class for WatchWindow");
        return 0;
    }

    /* Create an invisible window */
    hwndMain = CreateWindow(wc.lpszClassName, 
			    service_name ? (char *) service_name : "Apache", 
                            WS_OVERLAPPEDWINDOW & ~WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 
                            CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, NULL, NULL);

    if (!hwndMain)
    {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                     "Could not create WatchWindow");
        return 0;
    }
    
    while (GetMessage(&msg, NULL, 0, 0)) 
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    /* unhook window messages - ugly */
//  if (catch_term_hook)
//      UnhookWindowsHookEx(catch_term_hook);

    return 0;
}

/* This function only works on Win9x, and only when this process
 * is the active process (e.g. once it is running a child process,
 * it can no longer determine which console window is its own.)
 */
static BOOL CALLBACK EnumttyWindow(HWND wnd, LPARAM retwnd)
{
    char tmp[20], *tty;
    if (isWindowsNT())
        tty = "ConsoleWindowClass";
    else
        tty = "tty";
    if (GetClassName(wnd, tmp, sizeof(tmp)) && !strcmp(tmp, tty)) 
    {
        DWORD wndproc, thisproc = GetCurrentProcessId();
        GetWindowThreadProcessId(wnd, &wndproc);
        if (wndproc == thisproc) {
            *((HWND*)retwnd) = wnd;
            return FALSE;
        }
    }
    return TRUE;
}

void stop_child_monitor(void)
{
    if (isWindowsNT()) {
        SetConsoleCtrlHandler(ap_control_handler, FALSE);
        return;
    }

    if (monitor_thread_id)
        PostThreadMessage(monitor_thread_id, WM_QUIT, 0, 0);

    if (dos_child_procid) {
        RegisterServiceProcess(dos_child_procid, 0);
        dos_child_procid = 0;
    }

    /* When the service quits, remove it from the 
       system service table */
    RegisterServiceProcess(0, 0);
    
    /* Free the kernel library */
    FreeLibrary(monitor_hkernel);
}

/*
 * The Win32 Apache child cannot loose its console since 16bit cgi 
 * processes will hang (9x) or fail (NT) if they are not launched 
 * from a 32bit console app into that app's console window.  
 * Mark the 9x child as a service process and let the parent process 
 * clean it up as necessary.
 */
void ap_start_child_console(int is_child_of_service)
{
    HANDLE thread;

    is_service = 0; /* The child is never exactly a service */

    if (!is_child_of_service)
        die_on_logoff = TRUE;

    if (isWindowsNT()) {
        if (!is_child_of_service) {
            /*
             * Console mode Apache/WinNT needs to detach from the parent
             * console and create and hide it's own console window.
             * Not only is logout and shutdown more stable under W2K,
             * but this eliminates the mystery 'flicker' that users see
             * when invoking CGI apps (e.g. the titlebar or icon of the
             * console window changing to the cgi process's identifiers.)
             */
            FreeConsole();
            AllocConsole();
            EnumWindows(EnumttyWindow, (long)(&console_wnd));
            if (console_wnd)
                ShowWindow(console_wnd, SW_HIDE);
        }
        /*
         * Apache/WinNT installs no child console handler, otherwise
         * logoffs interfere with the service's child process!
         * The child process must have a later shutdown priority
         * than the parent, or the parent cannot shut down the
         * child process properly.  (The parent's default is 0x280.)
         */
        SetProcessShutdownParameters(0x200, 0);
        return;
    }

    /* Obtain a handle to the kernel library */
    monitor_hkernel = LoadLibrary("KERNEL32.DLL");
    if (!monitor_hkernel)
        return;
    
    /* Find the RegisterServiceProcess function */
    RegisterServiceProcess = (DWORD (WINAPI *)(DWORD, DWORD))
                   GetProcAddress(monitor_hkernel, "RegisterServiceProcess");
    if (RegisterServiceProcess == NULL)
        return;
	
    /* Register this process as a service */
    if (!RegisterServiceProcess(0, 1))
        return;

    if (!is_child_of_service) {
        /*
         * Needs thorough testing, although the idea makes sense;
         * Console mode Apache/Win9x might benefit from detaching
         * from the parent console, creating and hiding it's own 
         * console window.  No noticable difference yet, except
         * that the flicker (when executing CGIs) does disappear.
         */
        FreeConsole();
        AllocConsole();
    }

    EnumWindows(EnumttyWindow, (long)(&console_wnd));
    if (console_wnd && !is_child_of_service)
    {
        /*
         * Hide our newly created child console
         */
        ShowWindow(console_wnd, SW_HIDE);
    }
    if (console_wnd)
    {
        /*
         * Locate our winoldap process, and tag it as a service process 
         */
        HWND console_child = GetWindow(console_wnd, GW_CHILD);
        GetWindowThreadProcessId(console_child, &dos_child_procid);
        if (dos_child_procid)
            if (!RegisterServiceProcess(dos_child_procid, 1))
                dos_child_procid = 0;
    }

    /* WatchWindow is NULL - that is - we are not the actual service,
     * so we are not monitoring these events, nor will we respond to
     * service requests to our window.
     */
    thread = CreateThread(NULL, 0, WatchWindow, NULL, 0, 
			  &monitor_thread_id);
    if (thread)
        CloseHandle(thread);

    atexit(stop_child_monitor);
}


void stop_console_monitor(void)
{
    if (!isWindowsNT() && monitor_thread_id)
	PostThreadMessage(monitor_thread_id, WM_QUIT, 0, 0);

    /* Remove the control handler at the end of the day. */
    SetConsoleCtrlHandler(ap_control_handler, FALSE);
}

void ap_start_console_monitor(void)
{
    HANDLE console_input;
 
    is_service = 0;

    console_input = GetStdHandle(STD_INPUT_HANDLE);
    /* Assure we properly accept Ctrl+C as an interrupt...
     * Win/2000 definately makes some odd assumptions about 
     * ctrl+c and the reserved console mode bits!
     */
    if (console_input != INVALID_HANDLE_VALUE)
    {
        /* The SetConsoleCtrlHandler(NULL... would fault under Win9x 
         * WinNT also includes an undocumented 0x80 bit for console mode
         * that preserves the console window behavior, and prevents the
         * bogus 'selection' mode from being accedently triggered.
         */
        if (isWindowsNT()) {
	    SetConsoleCtrlHandler(NULL, FALSE);
            SetConsoleMode(console_input, ENABLE_LINE_INPUT 
                                        | ENABLE_ECHO_INPUT 
                                        | ENABLE_PROCESSED_INPUT
                                        | 0x80);
        }
	else {
            SetConsoleMode(console_input, ENABLE_LINE_INPUT 
                                        | ENABLE_ECHO_INPUT 
                                        | ENABLE_PROCESSED_INPUT);
        }
        SetConsoleCtrlHandler(ap_control_handler, TRUE);
    }
    
    /* Under 95/98 create a monitor window to watch for session end,
     * pass NULL to WatchWindow so we do not appear to be a service.
     */
    if (!isWindowsNT()) {
        HANDLE thread;
        thread = CreateThread(NULL, 0, WatchWindow, NULL, 0, 
                              &monitor_thread_id);
        if (thread)
            CloseHandle(thread);
    }

    atexit(stop_console_monitor);
}

void stop_service_monitor(void)
{
    if (monitor_thread_id)
        PostThreadMessage(monitor_thread_id, WM_QUIT, 0, 0);

    /* When the service quits, remove it from the 
       system service table */
    RegisterServiceProcess(0, 0);
    
    if (dos_child_procid) {
        RegisterServiceProcess(dos_child_procid, 0);
        dos_child_procid = 0;
    }

    /* Free the kernel library */
    FreeLibrary(monitor_hkernel);
}

int service95_main(int (*main_fn)(int, char **), int argc, char **argv, 
		   char *display_name)
{
    /* Windows 95/98 */
    char *service_name;
    HANDLE thread;

    is_service = 1; /* The child is never exactly a service */

    /* Set up the Win9x server name, as WinNT would */
    ap_server_argv0 = globdat.name = display_name;

    /* Remove spaces from display name to create service name */
    service_name = strdup(display_name);
    ap_remove_spaces(service_name, display_name);

    /* Obtain a handle to the kernel library */
    monitor_hkernel = LoadLibrary("KERNEL32.DLL");
    if (!monitor_hkernel)
        return -1;
    
    /* Find the RegisterServiceProcess function */
    RegisterServiceProcess = (DWORD (WINAPI *)(DWORD, DWORD))
                   GetProcAddress(monitor_hkernel, "RegisterServiceProcess");
    if (RegisterServiceProcess == NULL)
        return -1;
	
    /* Register this process as a service */
    if (!RegisterServiceProcess(0, 1))
        return -1;

    /* Prevent holding open the (nonexistant) console */
    real_exit_code = 0;

    /* Hide the console of this Apache parent process
     * (children must have a parent in order to properly execute
     * 16-bit CGI processes or they will lock.)
     */
    FreeConsole();

    thread = CreateThread(NULL, 0, WatchWindow, (LPVOID) service_name, 0, 
			  &monitor_thread_id);
    if (thread)
        CloseHandle(thread);

    atexit(stop_service_monitor);
    
    /* Run the service */
    globdat.exit_status = main_fn(argc, argv);
}

int service_main(int (*main_fn)(int, char **), int argc, char **argv )
{
    SERVICE_TABLE_ENTRY dispatchTable[] =
    {
        { "", service_main_fn },
        { NULL, NULL }
    };

    /* Prevent holding open the (nonexistant) console */
    real_exit_code = 0;

    globdat.main_fn = main_fn;
    globdat.stop_event = create_event(0, 0, "apache-signal");
    globdat.connected = 1;

    if(!StartServiceCtrlDispatcher(dispatchTable))
    {
        /* This is a genuine failure of the SCM. */
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
        "Error starting service control dispatcher");
        return(globdat.exit_status);
    }
    else
    {
        return(globdat.exit_status);
    }
}

void service_cd()
{
    /* change to the drive and directory with the executable */
    char buf[300], *p;
    GetModuleFileName(NULL, buf, 300);
    p = strrchr(buf, '\\');
    if (p != NULL)
        *p = 0;
    chdir(buf);
}

long __stdcall service_stderr_thread(LPVOID hPipe)
{
    HANDLE hPipeRead = (HANDLE) hPipe;
    HANDLE hEventSource;
    char errbuf[256];
    char *errmsg = errbuf;
    char *errarg[9];
    DWORD errlen = 0;
    DWORD errres;
    HKEY hk;
    
    errarg[0] = "The Apache service named";
    errarg[1] = ap_server_argv0;
    errarg[2] = "reported the following error:\r\n>>>";
    errarg[3] = errmsg;
    errarg[4] = "<<<\r\n before the error.log file could be opened.\r\n";
    errarg[5] = "More information may be available in the error.log file.";
    errarg[6] = NULL;
    errarg[7] = NULL;
    errarg[8] = NULL;
    
    /* What are we going to do in here, bail on the user?  not. */
    if (!RegCreateKey(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services"
                      "\\EventLog\\Application\\Apache Service", &hk)) 
    {
        /* The stock message file */
        char *netmsgkey = "%SystemRoot%\\System32\\netmsg.dll";
        DWORD dwData = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | 
                       EVENTLOG_INFORMATION_TYPE; 
 
        RegSetValueEx(hk, "EventMessageFile", 0, REG_EXPAND_SZ,
                          (LPBYTE) netmsgkey, strlen(netmsgkey) + 1);
        
        RegSetValueEx(hk, "TypesSupported", 0, REG_DWORD,
                          (LPBYTE) &dwData, sizeof(dwData));
        RegCloseKey(hk);
    }

    hEventSource = RegisterEventSource(NULL, "Apache Service");

    while (ReadFile(hPipeRead, errmsg, 1, &errres, NULL) && (errres == 1))
    {
        if ((errmsg > errbuf) || !isspace(*errmsg))
        {
            ++errlen;
            ++errmsg;
            if ((*(errmsg - 1) == '\n') || (errlen == sizeof(errbuf) - 1))
            {
                while (errlen && isspace(errbuf[errlen - 1]))
                    --errlen;
                errbuf[errlen] = '\0';

                /* Generic message: '%1 %2 %3 %4 %5 %6 %7 %8 %9'
                 * The event code in netmsg.dll is 3299
                 */
                ReportEvent(hEventSource, EVENTLOG_ERROR_TYPE, 0, 
                            3299, NULL, 9, 0, errarg, NULL);
                errmsg = errbuf;
                errlen = 0;
            }
        }
    }

    CloseHandle(hPipeRead);
    return 0;
}

void __stdcall service_main_fn(DWORD argc, LPTSTR *argv)
{
    HANDLE hCurrentProcess;
    HANDLE hPipeRead = NULL;
    HANDLE hPipeWrite = NULL;
    HANDLE hPipeReadDup;
    HANDLE thread;
    DWORD  threadid;
    SECURITY_ATTRIBUTES sa = {0};  
    
    ap_server_argv0 = globdat.name = argv[0];

    is_service = 1;

    if(!(globdat.hServiceStatus = RegisterServiceCtrlHandler(globdat.name, 
                                                             service_ctrl)))
    {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
        "Failure registering service handler");
        return;
    }

    ReportStatusToSCMgr(
        SERVICE_START_PENDING, // service state
        NO_ERROR,              // exit code
        3000);                 // wait hint

    /* Create a pipe to send stderr messages to the system error log */
    hCurrentProcess = GetCurrentProcess();
    if (CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0)) 
    {
        if (DuplicateHandle(hCurrentProcess, hPipeRead, hCurrentProcess,
                            &hPipeReadDup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        {
            CloseHandle(hPipeRead);
            hPipeRead = hPipeReadDup;
            thread = CreateThread(NULL, 0, service_stderr_thread, 
                                  (LPVOID) hPipeRead, 0, &threadid);
            if (thread)
            {
                int fh;
                FILE *fl;
                CloseHandle(thread);
            	fflush(stderr);
		SetStdHandle(STD_ERROR_HANDLE, hPipeWrite);
				
                fh = _open_osfhandle((long) STD_ERROR_HANDLE, 
                                     _O_WRONLY | _O_BINARY);
                dup2(fh, STDERR_FILENO);
                fl = _fdopen(STDERR_FILENO, "wcb");
                memcpy(stderr, fl, sizeof(FILE));
            }
            else
            {
                CloseHandle(hPipeRead);
                CloseHandle(hPipeWrite);
                hPipeWrite = NULL;
            }            
        }
        else
        {
            CloseHandle(hPipeRead);
            CloseHandle(hPipeWrite);
            hPipeWrite = NULL;
        }            
    }

    service_cd();
    if( service_init() ) 
        /* Arguments are ok except for \! */
        globdat.exit_status = (*globdat.main_fn)( argc, argv );

    if (hPipeWrite)
        CloseHandle(hPipeWrite);

    ReportStatusToSCMgr(SERVICE_STOPPED, NO_ERROR, 0);

    return;
}


void service_set_status(int status)
{
    ReportStatusToSCMgr(status, NO_ERROR, 3000);
}



//
//  FUNCTION: service_ctrl
//
//  PURPOSE: This function is called by the SCM whenever
//           ControlService() is called on this service.
//
//  PARAMETERS:
//    dwCtrlCode - type of control requested
//
//  RETURN VALUE:
//    none
//
//  COMMENTS:
//
VOID WINAPI service_ctrl(DWORD dwCtrlCode)
{
    int state;


    state = globdat.ssStatus.dwCurrentState;
    switch(dwCtrlCode)
    {
        // Stop the service.
        //
        case SERVICE_CONTROL_STOP:
            state = SERVICE_STOP_PENDING;
	    ap_start_shutdown();
            break;

        case SERVICE_APACHE_RESTART:
            state = SERVICE_START_PENDING;
	    ap_start_restart(1);
            break;

        // Update the service status.
        //
        case SERVICE_CONTROL_INTERROGATE:
            break;

        // invalid control code
        //
        default:
            break;

    }

    ReportStatusToSCMgr(state, NO_ERROR, 0);
}


int ReportStatusToSCMgr(int currentState, int exitCode, int waitHint)
{
    static int firstTime = 1;
    static int checkPoint = 1;
    int rv;
    
    if(firstTime)
    {
        firstTime = 0;
        globdat.ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        globdat.ssStatus.dwServiceSpecificExitCode = 0;
        globdat.ssStatus.dwCheckPoint = 1;
    }

    if(globdat.connected)
    {
        if (currentState == SERVICE_START_PENDING)
            globdat.ssStatus.dwControlsAccepted = 0;
        else
            globdat.ssStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

        globdat.ssStatus.dwCurrentState = currentState;
        globdat.ssStatus.dwWin32ExitCode = exitCode;
        if(waitHint)
            globdat.ssStatus.dwWaitHint = waitHint;

        if ( ( currentState == SERVICE_RUNNING ) ||
             ( currentState == SERVICE_STOPPED ) )
        {
            globdat.ssStatus.dwWaitHint = 0;
            globdat.ssStatus.dwCheckPoint = 0;
        }
        else
            globdat.ssStatus.dwCheckPoint = ++checkPoint;

        rv = SetServiceStatus(globdat.hServiceStatus, &globdat.ssStatus);

    }
    return(1);
}

void InstallService(char *display_name, char *conf)
{
    TCHAR szPath[MAX_PATH];
    TCHAR szQuotedPath[512];
    char *service_name;

    printf("Installing the %s service to use %s\n", display_name, conf);

    if (GetModuleFileName( NULL, szPath, 512 ) == 0)
    {
        ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
        "GetModuleFileName failed");
        return;
    }

    /* Remove spaces from display name to create service name */
    service_name = strdup(display_name);
    ap_remove_spaces(service_name, display_name);

    if (isWindowsNT())
    {
        SC_HANDLE schService;
        SC_HANDLE schSCManager;

        ap_snprintf(szQuotedPath, sizeof(szQuotedPath), "\"%s\" --ntservice", szPath);

        schSCManager = OpenSCManager(
                            NULL,                 // machine (local)
                            NULL,                 // database (default)
                            SC_MANAGER_ALL_ACCESS // access required
                            );
       if (!schSCManager) {
           ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
	       "OpenSCManager failed");
           return;
        }
        
        /* Added dependencies for the following: TCPIP, AFD
         * AFD is the winsock handler, TCPIP is self evident
         *
         * RPCSS is the Remote Procedure Call (RPC) Locator
         * required for DCOM communication.  I am far from
         * convinced we should toggle this, but be warned that
         * future apache modules or ISAPI dll's may depend on it.
         */
        schService = CreateService(
            schSCManager,               // SCManager database
            service_name,               // name of service
            display_name,               // name to display
            SERVICE_ALL_ACCESS,         // desired access
            SERVICE_WIN32_OWN_PROCESS,  // service type
            SERVICE_AUTO_START,         // start type
            SERVICE_ERROR_NORMAL,       // error control type
            szQuotedPath,               // service's binary
            NULL,                       // no load ordering group
            NULL,                       // no tag identifier
            "Tcpip\0Afd\0",             // dependencies
            NULL,                       // LocalSystem account
            NULL);                      // no password

        if (schService) {
            CloseServiceHandle(schService);

        }
        else {
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL, 
		"CreateService failed");
        }

        CloseServiceHandle(schSCManager);
    }
    else /* !isWindowsNT() */
    {
        HKEY hkey;
        DWORD rv;

        ap_snprintf(szQuotedPath, sizeof(szQuotedPath),
                    "\"%s\" -k start -n %s", 
                    szPath, service_name);
        /* Create/Find the RunServices key */
        rv = RegCreateKey(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows"
                          "\\CurrentVersion\\RunServices", &hkey);
        if (rv != ERROR_SUCCESS) {
            SetLastError(rv);
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "Could not create/open the RunServices registry key");
            return;
        }

        /* Attempt to add the value for our service */
        rv = RegSetValueEx(hkey, service_name, 0, REG_SZ, 
                           (unsigned char *)szQuotedPath, 
                           strlen(szQuotedPath) + 1);
        if (rv != ERROR_SUCCESS) {
            SetLastError(rv);
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "Unable to install service: "
                         "Could not add to RunServices Registry Key");
            RegCloseKey(hkey);
            return;
        }

        RegCloseKey(hkey);

        /* Create/Find the Service key for Monitor Applications to iterate */
        ap_snprintf(szPath, sizeof(szPath), 
                    "SYSTEM\\CurrentControlSet\\Services\\%s", service_name);
        rv = RegCreateKey(HKEY_LOCAL_MACHINE, szPath, &hkey);
        if (rv != ERROR_SUCCESS) {
            SetLastError(rv);
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "Could not create/open the %s registry key", szPath);
            return;
        }

        /* Attempt to add the ImagePath value to identify it as Apache */
        rv = RegSetValueEx(hkey, "ImagePath", 0, REG_SZ, 
                           (unsigned char *)szQuotedPath, 
                           strlen(szQuotedPath) + 1);
        if (rv != ERROR_SUCCESS) {
            SetLastError(rv);
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "Unable to install service: "
                         "Could not add ImagePath to %s Registry Key", 
                         service_name);
            RegCloseKey(hkey);
            return;
        }

        /* Attempt to add the DisplayName value for our service */
        rv = RegSetValueEx(hkey, "DisplayName", 0, REG_SZ, 
                           (unsigned char *)display_name, 
                           strlen(display_name) + 1);
        if (rv != ERROR_SUCCESS) {
            SetLastError(rv);
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "Unable to install service: "
                         "Could not add DisplayName to %s Registry Key", 
                         service_name);
            RegCloseKey(hkey);
            return;
        }

        RegCloseKey(hkey);
    }

    /* Both Platforms: Now store the server_root in the registry */
    if(!ap_registry_set_service_conf(conf, service_name))
        printf("The %s service has been installed successfully.\n", display_name);
}

void RemoveService(char *display_name)
{
    char *service_name;
    BOOL success = FALSE;

    printf("Removing the %s service\n", display_name);

    /* Remove spaces from display name to create service name */
    service_name = strdup(display_name);
    ap_remove_spaces(service_name, display_name);

    if (isWindowsNT())
    {
        SC_HANDLE   schService;
        SC_HANDLE   schSCManager;
    
        schSCManager = OpenSCManager(
                            NULL,                   // machine (NULL == local)
                            NULL,                   // database (NULL == default)
                            SC_MANAGER_ALL_ACCESS   // access required
                            );
        if (!schSCManager) {
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "OpenSCManager failed");
            return;
        }

        schService = OpenService(schSCManager, service_name, SERVICE_ALL_ACCESS);

        if (schService == NULL) {
            /* Could not open the service */
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "OpenService failed");
        }
        else {
            /* try to stop the service */
            ap_stop_service(schService);

            // now remove the service
            if (DeleteService(schService) == 0)
                ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                             "DeleteService failed");
            else
                success = TRUE;
            CloseServiceHandle(schService);
        }
        /* SCM removes registry parameters  */
        CloseServiceHandle(schSCManager);
    }
    else /* !isWindowsNT() */
    {
        HKEY hkey;
        DWORD rv;

        /* Open the RunServices key */
        rv = RegOpenKey(HKEY_LOCAL_MACHINE, 
                "Software\\Microsoft\\Windows\\CurrentVersion\\RunServices",
                    &hkey);
        if (rv != ERROR_SUCCESS) {
            SetLastError(rv);
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "Could not open the RunServices registry key.");
        } 
        else {
            /* Delete the registry value for this service */
            rv = RegDeleteValue(hkey, service_name);
            if (rv != ERROR_SUCCESS) {
                SetLastError(rv);
                ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                             "Unable to remove service: "
                             "Could not delete the RunServices entry.");
            }
            else
                success = TRUE;
        }
        RegCloseKey(hkey);

        /* Open the Services key */
        rv = RegOpenKey(HKEY_LOCAL_MACHINE, 
                        "SYSTEM\\CurrentControlSet\\Services", &hkey);
        if (rv != ERROR_SUCCESS) {
            rv = RegDeleteValue(hkey, service_name);
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "Could not open the Services registry key.");
            success = FALSE;
        } 
        else {
            /* Delete the registry key for this service */
            rv = RegDeleteKey(hkey, service_name);
            if (rv != ERROR_SUCCESS) {
                SetLastError(rv);
                ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                             "Unable to remove service: "
                             "Could not delete the Services registry key.");
                success = FALSE;
            }
        }
        RegCloseKey(hkey);
    }
    if (success)
        printf("The %s service has been removed successfully.\n", 
               display_name);
}

/*
 * A hack to determine if we're running as a service without waiting for
 * the SCM to fail.
 */

BOOL isProcessService() 
{
    if (is_service != -1)
        return is_service;
    if (!isWindowsNT() || !AllocConsole()) {
        /* Don't assume anything, just yet */
        return FALSE;
    }
    FreeConsole();
    is_service = 1;
    return TRUE;
}

/* Determine is service_name is a valid service
 *
 * TODO: be nice if we tested that it is an 'apache' service, no?
 */

BOOL isValidService(char *display_name) {
    char service_key[MAX_PATH];
    char *service_name;
    HKEY hkey;
    
    /* Remove spaces from display name to create service name */
    strcpy(service_key, "System\\CurrentControlSet\\Services\\");
    service_name = strchr(service_key, '\0');
    ap_remove_spaces(service_name, display_name);

    if (RegOpenKey(HKEY_LOCAL_MACHINE, service_key, &hkey) != ERROR_SUCCESS) {
        return FALSE;
    }
    RegCloseKey(hkey);
    return TRUE;
}

BOOL isWindowsNT(void)
{
    static BOOL once = FALSE;
    static BOOL isNT = FALSE;
    
    if (!once)
    {
        OSVERSIONINFO osver;
        osver.dwOSVersionInfoSize = sizeof(osver);
        if (GetVersionEx(&osver))
            if (osver.dwPlatformId == VER_PLATFORM_WIN32_NT)
                isNT = TRUE;
        once = TRUE;
    }
    return isNT;
}

int send_signal_to_service(char *display_name, char *sig) 
{
    DWORD       service_pid;
    HANDLE      hwnd;
    SC_HANDLE   schService;
    SC_HANDLE   schSCManager;
    char       *service_name;
    int success = FALSE;

    enum                        { start,      restart,      stop, unknown } action;
    static char *param[] =      { "start",    "restart",    "shutdown" };
    static char *participle[] = { "starting", "restarting", "stopping" };
    static char *past[]       = { "started",  "restarted",  "stopped"  };

    for (action = start; action < unknown; action++)
        if (!strcasecmp(sig, param[action]))
            break;

    if (action == unknown) {
        printf("signal must be start, restart, or shutdown\n");
        return FALSE;
    }

    /* Remove spaces from display name to create service name */
    service_name = strdup(display_name);
    ap_remove_spaces(service_name, display_name);

    if (isWindowsNT()) 
    {
        schSCManager = OpenSCManager(
                            NULL,                   // machine (NULL == local)
                            NULL,                   // database (NULL == default)
                            SC_MANAGER_ALL_ACCESS   // access required
                            );
        if (!schSCManager) {
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "OpenSCManager failed");
            return FALSE;
        }
        
        schService = OpenService(schSCManager, service_name, SERVICE_ALL_ACCESS);

        if (schService == NULL) {
            /* Could not open the service */
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "OpenService failed");
            CloseServiceHandle(schSCManager);
            return FALSE;
        }
        
        if (!QueryServiceStatus(schService, &globdat.ssStatus)) {
            ap_log_error(APLOG_MARK, APLOG_ERR|APLOG_WIN32ERROR, NULL,
                         "QueryService failed");
            CloseServiceHandle(schService);
            CloseServiceHandle(schSCManager);
        }
    }
    else /* !isWindowsNT() */
    {
        /* Locate the window named service_name of class ApacheWin95ServiceMonitor
         * from the active top level windows
         */
        hwnd = FindWindow("ApacheWin95ServiceMonitor", service_name);
        if (hwnd && GetWindowThreadProcessId(hwnd, &service_pid))
            globdat.ssStatus.dwCurrentState = SERVICE_RUNNING;
        else
            globdat.ssStatus.dwCurrentState = SERVICE_STOPPED;
    }

    if (globdat.ssStatus.dwCurrentState == SERVICE_STOPPED 
            && action == stop) {
        printf("The %s service is not started.\n", display_name);
        return FALSE;
    }
    else if (globdat.ssStatus.dwCurrentState == SERVICE_RUNNING 
                 && action == start) {
        printf("The %s service has already been started.\n", display_name);
        strcpy(sig, "");
        return FALSE;
    }
    else
    {
        printf("The %s service is %s.\n", display_name, participle[action]);

        if (isWindowsNT()) 
        {
            if (action == stop)
                success = ap_stop_service(schService);
            else if ((action == start) 
                     || ((action == restart) 
                            && (globdat.ssStatus.dwCurrentState 
                                    == SERVICE_STOPPED)))
                success = ap_start_service(schService);
            else if (action == restart)
                success = ap_restart_service(schService);
        }
        else /* !isWindowsNT()) */
        {
            char prefix[20];
            ap_snprintf(prefix, sizeof(prefix), "ap%ld", (long)service_pid);
            setup_signal_names(prefix);

            if (action == stop) {
                int ticks = 60;
                ap_start_shutdown();
                while (--ticks)
                {
                    if (!IsWindow(hwnd)) {
                        success = TRUE;
                        break;
                    }
                    Sleep(1000);
                }
            }
            else if (action == restart) 
            {   
                /* This gets a bit tricky... start and restart (of stopped service)
                 * will simply fall through and *THIS* process will fade into an
                 * invisible 'service' process, detaching from the user's console.
                 * We need to change the restart signal to "start", however,
                 * if the service was not -yet- running, and we do return FALSE
                 * to assure main() that we haven't done anything yet.
                 */
                if (globdat.ssStatus.dwCurrentState == SERVICE_STOPPED) 
                {
                    printf("The %s service has %s.\n", display_name, 
                           past[action]);
                    strcpy(sig, "start");
                    return FALSE;
                }
                
                ap_start_restart(1);
                success = TRUE;
            }
            else /* action == start */
            {
                printf("The %s service is %s.\n", display_name, 
                       past[action]);
                return FALSE;
            }
        }

        if( success )
            printf("The %s service has %s.\n", display_name, past[action]);
        else
            printf("Failed to %s the %s service.\n", sig, display_name);
    }

    if (isWindowsNT()) {
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
    }
    return success;
}

int ap_stop_service(SC_HANDLE schService)
{
    if (ControlService(schService, SERVICE_CONTROL_STOP, &globdat.ssStatus)) {
        Sleep(1000);
        while (QueryServiceStatus(schService, &globdat.ssStatus)) {
            if (globdat.ssStatus.dwCurrentState == SERVICE_STOP_PENDING)
                Sleep(1000);
            else
                break;
        }
    }
    if (QueryServiceStatus(schService, &globdat.ssStatus))
        if (globdat.ssStatus.dwCurrentState == SERVICE_STOPPED)
            return TRUE;
    return FALSE;
}

int ap_start_service(SC_HANDLE schService) {
    if (StartService(schService, 0, NULL)) {
        Sleep(1000);
        while(QueryServiceStatus(schService, &globdat.ssStatus)) {
            if(globdat.ssStatus.dwCurrentState == SERVICE_START_PENDING)
                Sleep(1000);
            else
                break;
        }
    }
    if (QueryServiceStatus(schService, &globdat.ssStatus))
        if (globdat.ssStatus.dwCurrentState == SERVICE_RUNNING)
            return TRUE;
    return FALSE;
}

int ap_restart_service(SC_HANDLE schService) 
{
    int ticks;
    if (ControlService(schService, SERVICE_APACHE_RESTART, &globdat.ssStatus)) 
    {
        ticks = 60;
        while (globdat.ssStatus.dwCurrentState == SERVICE_START_PENDING)
        {
            Sleep(1000);
            if (!QueryServiceStatus(schService, &globdat.ssStatus)) 
                return FALSE;
            if (!--ticks)
                break;
        }
    }
    if (globdat.ssStatus.dwCurrentState == SERVICE_RUNNING)
        return TRUE;
    return FALSE;
}
#endif /* WIN32 */


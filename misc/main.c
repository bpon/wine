/*
 * Main function.
 *
 * Copyright 1994 Alexandre Julliard
 *
static char Copyright[] = "Copyright  Alexandre Julliard, 1994";
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>
#ifdef MALLOC_DEBUGGING
#include <malloc.h>
#endif
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include "wine.h"
#include "msdos.h"
#include "windows.h"
#include "comm.h"
#include "miscemu.h"
#include "winsock.h"
#include "options.h"
#include "dos_fs.h"
#include "desktop.h"
#include "prototypes.h"
#include "texts.h"
#include "library.h"
#include "dlls.h"
#include "if1632.h"
#define DEBUG_DEFINE_VARIABLES
#include "stddebug.h"
#include "debug.h"

extern ButtonTexts ButtonText;

static const char people[] = "Wine is available thanks to the work of "\
"Bob Amstadt, Dag Asheim, Martin Ayotte, Erik Bos, John Brezak, "\
"Andrew Bulhak, John Burton, Paul Falstad, Peter Galbavy, Jeffrey Hsu, "\
"Miguel de Icaza, Alexandre Julliard, Jon Konrath, Scott A. Laird, "\
"Martin von Loewis, Kenneth MacDonald, Peter MacDonald, David Metcalfe, "\
"Michael Patra, John Richardson, Johannes Ruscheinski, Yngvi Sigurjonsson, "\
"Rick Sladkey, William Smith, Jon Tombs, Linus Torvalds, Carl Williams, "\
"Karl Guenter Wuensch, and Eric Youngdale.";

#define WINE_CLASS    "Wine"    /* Class name for resources */

typedef struct tagENVENTRY {
	LPSTR				Name;
	LPSTR				Value;
	WORD				wSize;
	struct tagENVENTRY	*Prev;
	struct tagENVENTRY	*Next;
	} ENVENTRY;
typedef ENVENTRY *LPENVENTRY;

LPENVENTRY	lpEnvList = NULL;

Display *display;
Screen *screen;
Window rootWindow;
int screenWidth = 0, screenHeight = 0;  /* Desktop window dimensions */
int screenDepth = 0;  /* Screen depth to use */
int desktopX = 0, desktopY = 0;  /* Desktop window position (if any) */

struct options Options =
{  /* default options */
    NULL,           /* spyFilename */
    NULL,           /* desktopGeometry */
    NULL,           /* programName */
    FALSE,          /* usePrivateMap */
    FALSE,          /* synchronous */
    FALSE,          /* backing store */
    SW_SHOWNORMAL,  /* cmdShow */
    FALSE,
    FALSE           /* AllowReadOnly */
};


static XrmOptionDescRec optionsTable[] =
{
    { "-backingstore",  ".backingstore",    XrmoptionNoArg,  (caddr_t)"on" },
    { "-desktop",       ".desktop",         XrmoptionSepArg, (caddr_t)NULL },
    { "-depth",         ".depth",           XrmoptionSepArg, (caddr_t)NULL },
    { "-display",       ".display",         XrmoptionSepArg, (caddr_t)NULL },
    { "-iconic",        ".iconic",          XrmoptionNoArg,  (caddr_t)"on" },
    { "-name",          ".name",            XrmoptionSepArg, (caddr_t)NULL },
    { "-privatemap",    ".privatemap",      XrmoptionNoArg,  (caddr_t)"on" },
    { "-synchronous",   ".synchronous",     XrmoptionNoArg,  (caddr_t)"on" },
    { "-spy",           ".spy",             XrmoptionSepArg, (caddr_t)NULL },
    { "-debug",         ".debug",           XrmoptionNoArg,  (caddr_t)"on" },
    { "-debugmsg",      ".debugmsg",        XrmoptionSepArg, (caddr_t)NULL },
    { "-dll",           ".dll",             XrmoptionSepArg, (caddr_t)NULL },
    { "-allowreadonly", ".allowreadonly",   XrmoptionNoArg,  (caddr_t)"on" }
};

#define NB_OPTIONS  (sizeof(optionsTable) / sizeof(optionsTable[0]))

#define USAGE \
  "Usage:  %s [options] program_name [arguments]\n" \
  "\n" \
  "Options:\n" \
  "    -depth n        Change the depth to use for multiple-depth screens\n" \
  "    -desktop geom   Use a desktop window of the given geometry\n" \
  "    -display name   Use the specified display\n" \
  "    -iconic         Start as an icon\n" \
  "    -debug          Enter debugger before starting application\n" \
  "    -name name      Set the application name\n" \
  "    -privatemap     Use a private color map\n" \
  "    -synchronous    Turn on synchronous display mode\n" \
  "    -backingstore   Turn on backing store\n" \
  "    -spy file       Turn on message spying to the specified file\n" \
  "    -relaydbg       Obsolete. Use -debugmsg +relay instead\n" \
  "    -debugmsg name  Turn debugging-messages on or off\n" \
  "    -dll name       Enable or disable built-in DLLs\n" \
  "    -allowreadonly  Read only files may be opened in write mode\n"



/***********************************************************************
 *           MAIN_Usage
 */
static void MAIN_Usage( char *name )
{
    fprintf( stderr, USAGE, name );
    exit(1);
}


/***********************************************************************
 *           MAIN_GetProgramName
 *
 * Get the program name. The name is specified by (in order of precedence):
 * - the option '-name'.
 * - the environment variable 'WINE_NAME'.
 * - the last component of argv[0].
 */
static char *MAIN_GetProgramName( int argc, char *argv[] )
{
    int i;
    char *p;

    for (i = 1; i < argc-1; i++)
	if (!strcmp( argv[i], "-name" )) return argv[i+1];
    if ((p = getenv( "WINE_NAME" )) != NULL) return p;
    if ((p = strrchr( argv[0], '/' )) != NULL) return p+1;
    return argv[0];
}


/***********************************************************************
 *           MAIN_GetResource
 *
 * Fetch the value of resource 'name' using the correct instance name.
 * 'name' must begin with '.' or '*'
 */
static int MAIN_GetResource( XrmDatabase db, char *name, XrmValue *value )
{
    char *buff_instance, *buff_class;
    char *dummy;
    int retval;

    buff_instance = (char *)malloc(strlen(Options.programName)+strlen(name)+1);
    buff_class    = (char *)malloc( strlen(WINE_CLASS) + strlen(name) + 1 );

    strcpy( buff_instance, Options.programName );
    strcat( buff_instance, name );
    strcpy( buff_class, WINE_CLASS );
    strcat( buff_class, name );
    retval = XrmGetResource( db, buff_instance, buff_class, &dummy, value );
    free( buff_instance );
    free( buff_class );
    return retval;
}


/***********************************************************************
 *           MAIN_GetButtonText
 *
 * Fetch the value of resource 'name' using the correct instance name.
 * 'name' must begin with '.' or '*'
 *
 * The address of the string got from the XResoure is stored in Button.Label.
 * The corresponding hotkey is taken from this string.
 */

static void MAIN_GetButtonText( XrmDatabase db, char *name, ButtonDesc *Button)
{
    XrmValue value;
    char *i;

    if (MAIN_GetResource( db, name, &value))
      {
       Button->Label = value.addr;
       i = strchr(Button->Label,'&');
       if ( i == NULL )
         Button->Hotkey = '\0';
       else if ( i++ == '\0' )
         Button->Hotkey = '\0';
       else
         Button->Hotkey = *i;
      }
    Button->Hotkey = toupper(Button->Hotkey);
}

/***********************************************************************
 *           MAIN_GetAllButtonTexts
 *
 * Read all Button-labels from X11-resources if they exist.
 */

static void MAIN_GetAllButtonTexts(XrmDatabase db)
{
  MAIN_GetButtonText(db, ".YesLabel",    &ButtonText.Yes);
  MAIN_GetButtonText(db, ".NoLabel",     &ButtonText.No);
  MAIN_GetButtonText(db, ".OkLabel",     &ButtonText.Ok);
  MAIN_GetButtonText(db, ".CancelLabel", &ButtonText.Cancel);
  MAIN_GetButtonText(db, ".AbortLabel",  &ButtonText.Abort);
  MAIN_GetButtonText(db, ".RetryLabel",  &ButtonText.Retry);
  MAIN_GetButtonText(db, ".IgnoreLabel", &ButtonText.Ignore);
  MAIN_GetButtonText(db, ".CancelLabel", &ButtonText.Cancel);
}

/***********************************************************************
 *                    ParseDebugOptions
 *
 *  Turns specific debug messages on or off, according to "options".
 *  Returns TRUE if parsing was successfull 
 */
#ifdef DEBUG_RUNTIME

BOOL ParseDebugOptions(char *options)
{
  int l;
  if (strlen(options)<3)
    return FALSE;
  do
  {
    if ((*options!='+')&&(*options!='-'))
      return FALSE;
    if (strchr(options,','))
      l=strchr(options,',')-options;
    else
      l=strlen(options);
    if (!strncasecmp(options+1,"all",l-1))
      {
	int i;
	for (i=0;i<sizeof(debug_msg_enabled)/sizeof(short);i++)
	  debug_msg_enabled[i]=(*options=='+');
      }
    else
      {
	int i;
	for (i=0;i<sizeof(debug_msg_enabled)/sizeof(short);i++)
	  if (debug_msg_name && (!strncasecmp(options+1,debug_msg_name[i],l-1)))
	    {
	      debug_msg_enabled[i]=(*options=='+');
	      break;
	    }
	if (i==sizeof(debug_msg_enabled)/sizeof(short))
	  return FALSE;
      }
    options+=l;
  }
  while((*options==',')&&(*(++options)));
  if (*options)
    return FALSE;
  else
    return TRUE;
}

#endif

/***********************************************************************
 *           MAIN_ParseDLLOptions
 *
 * Set runtime DLL usage flags
 */
static BOOL MAIN_ParseDLLOptions(char *options)
{
  int l;
  int i;
  if (strlen(options)<3)
    return FALSE;
  do
  {
    if ((*options!='+')&&(*options!='-'))
      return FALSE;
    if (strchr(options,','))
      l=strchr(options,',')-options;
    else l=strlen(options);
    for (i=0;i<N_BUILTINS;i++)
         if (!strncasecmp(options+1,dll_builtin_table[i].dll_name,l-1))
           {
             dll_builtin_table[i].dll_is_used=(*options=='+');
             break;
           }
    if (i==N_BUILTINS)
         return FALSE;
    options+=l;
  }
  while((*options==',')&&(*(++options)));
  if (*options)
    return FALSE;
  else
    return TRUE;
}



/***********************************************************************
 *           MAIN_ParseOptions
 *
 * Parse command line options and open display.
 */
static void MAIN_ParseOptions( int *argc, char *argv[] )
{
    char *display_name;
    XrmValue value;
    XrmDatabase db = XrmGetFileDatabase("/usr/lib/X11/app-defaults/Wine");

      /* Parse command line */
    Options.programName = MAIN_GetProgramName( *argc, argv );
    XrmParseCommand( &db, optionsTable, NB_OPTIONS,
		     Options.programName, argc, argv );

#ifdef WINELIB
    /* Need to assemble command line and pass it to WinMain */
#else
    if (*argc < 2 || strcasecmp(argv[1], "-h") == 0) 
    	MAIN_Usage( argv[0] );
#endif

      /* Open display */

    if (MAIN_GetResource( db, ".display", &value )) display_name = value.addr;
    else display_name = NULL;

    if (!(display = XOpenDisplay( display_name )))
    {
	fprintf( stderr, "%s: Can't open display: %s\n",
		 argv[0], display_name ? display_name : "" );
	exit(1);
    }

      /* Get all options */
    if (MAIN_GetResource( db, ".iconic", &value ))
	Options.cmdShow = SW_SHOWMINIMIZED;
    if (MAIN_GetResource( db, ".privatemap", &value ))
	Options.usePrivateMap = TRUE;
    if (MAIN_GetResource( db, ".synchronous", &value ))
	Options.synchronous = TRUE;
    if (MAIN_GetResource( db, ".backingstore", &value ))
	Options.backingstore = TRUE;	
    if (MAIN_GetResource( db, ".debug", &value ))
	Options.debug = TRUE;
    if (MAIN_GetResource( db, ".allowreadonly", &value ))
        Options.allowReadOnly = TRUE;
    if (MAIN_GetResource( db, ".spy", &value))
	Options.spyFilename = value.addr;
    if (MAIN_GetResource( db, ".depth", &value))
	screenDepth = atoi( value.addr );
    if (MAIN_GetResource( db, ".desktop", &value))
	Options.desktopGeometry = value.addr;
#ifdef DEBUG_RUNTIME
    if (MAIN_GetResource( db, ".debugoptions", &value))
	ParseDebugOptions((char*)value.addr);
#endif
    if (MAIN_GetResource( db, ".debugmsg", &value))
      {
#ifndef DEBUG_RUNTIME
	fprintf(stderr,"%s: Option \"-debugmsg\" not implemented.\n" \
          "    Recompile with DEBUG_RUNTIME in include/stddebug.h defined.\n",
	  argv[0]);
	exit(1);
#else
	if (ParseDebugOptions((char*)value.addr)==FALSE)
	  {
	    int i;
	    fprintf(stderr,"%s: Syntax: -debugmsg +xxx,...  or -debugmsg -xxx,...\n",argv[0]);
	    fprintf(stderr,"Example: -debugmsg +all,-heap    turn on all messages except heap messages\n");
	    fprintf(stderr,"Available message types:\n");
	    fprintf(stderr,"%-9s ","all");
	    for(i=0;i<sizeof(debug_msg_enabled)/sizeof(short);i++)
	      if(debug_msg_name[i])
		fprintf(stderr,"%-9s%c",debug_msg_name[i],
			(((i+2)%8==0)?'\n':' '));
	    fprintf(stderr,"\n\n");
	    exit(1);
	  }
#endif
      }

      if(MAIN_GetResource( db, ".dll", &value))
       if(MAIN_ParseDLLOptions((char*)value.addr)==FALSE)
       {
         int i;
         fprintf(stderr,"%s: Syntax: -dll +xxx,... or -dll -xxx,...\n",argv[0]);
         fprintf(stderr,"Example: -dll -ole2    Do not use emulated OLE2.DLL\n");
         fprintf(stderr,"Available DLLs\n");
         for(i=0;i<N_BUILTINS;i++)
               fprintf(stderr,"%-9s%c",dll_builtin_table[i].dll_name,
                       (((i+2)%8==0)?'\n':' '));
         fprintf(stderr,"\n\n");
         exit(1);
       }

/*    MAIN_GetAllButtonTexts(db); */
 
}


/***********************************************************************
 *           MAIN_CreateDesktop
 */
static void MAIN_CreateDesktop( int argc, char *argv[] )
{
    int flags;
    unsigned int width = 640, height = 480;  /* Default size = 640x480 */
    char *name = "Wine desktop";
    XSizeHints *size_hints;
    XWMHints   *wm_hints;
    XClassHint *class_hints;
    XSetWindowAttributes win_attr;
    XTextProperty window_name;

    flags = XParseGeometry( Options.desktopGeometry,
			    &desktopX, &desktopY, &width, &height );
    screenWidth  = width;
    screenHeight = height;

      /* Create window */

    win_attr.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
	                 PointerMotionMask | ButtonPressMask |
			 ButtonReleaseMask | EnterWindowMask | 
			 StructureNotifyMask;
    win_attr.cursor = XCreateFontCursor( display, XC_top_left_arrow );

    rootWindow = XCreateWindow( display, DefaultRootWindow(display),
			        desktopX, desktopY, width, height, 0,
			        CopyFromParent, InputOutput, CopyFromParent,
			        CWEventMask | CWCursor, &win_attr );

      /* Set window manager properties */

    size_hints  = XAllocSizeHints();
    wm_hints    = XAllocWMHints();
    class_hints = XAllocClassHint();
    if (!size_hints || !wm_hints || !class_hints)
    {
        fprintf( stderr, "Not enough memory for window manager hints.\n" );
        exit(1);
    }
    size_hints->min_width = size_hints->max_width = width;
    size_hints->min_height = size_hints->max_height = height;
    size_hints->flags = PMinSize | PMaxSize;
    if (flags & (XValue | YValue)) size_hints->flags |= USPosition;
    if (flags & (WidthValue | HeightValue)) size_hints->flags |= USSize;
    else size_hints->flags |= PSize;

    wm_hints->flags = InputHint | StateHint;
    wm_hints->input = True;
    wm_hints->initial_state = NormalState;
    class_hints->res_name = argv[0];
    class_hints->res_class = "Wine";

    XStringListToTextProperty( &name, 1, &window_name );
    XSetWMProperties( display, rootWindow, &window_name, &window_name,
                      argv, argc, size_hints, wm_hints, class_hints );
    XFree( size_hints );
    XFree( wm_hints );
    XFree( class_hints );

      /* Map window */

    XMapWindow( display, rootWindow );
}


XKeyboardState keyboard_state;

/***********************************************************************
 *           MAIN_SaveSetup
 */
static void MAIN_SaveSetup(void)
{
    XGetKeyboardControl(display, &keyboard_state);
}

/***********************************************************************
 *           MAIN_RestoreSetup
 */
static void MAIN_RestoreSetup(void)
{
    XKeyboardControl keyboard_value;

    keyboard_value.key_click_percent	= keyboard_state.key_click_percent;
    keyboard_value.bell_percent 	= keyboard_state.bell_percent;
    keyboard_value.bell_pitch		= keyboard_state.bell_pitch;
    keyboard_value.bell_duration	= keyboard_state.bell_duration;
    keyboard_value.auto_repeat_mode	= keyboard_state.global_auto_repeat;

    XChangeKeyboardControl(display, KBKeyClickPercent | KBBellPercent | 
    	KBBellPitch | KBBellDuration | KBAutoRepeatMode, &keyboard_value);
}

static void malloc_error()
{
       fprintf(stderr,"malloc is not feeling well. Good bye\n");
       exit(1);
}

static void called_at_exit(void)
{
    Comm_DeInit();
    sync_profiles();
    MAIN_RestoreSetup();
    WSACleanup();
}

/***********************************************************************
 *           main
 */
int main( int argc, char *argv[] )
{    
    int ret_val;
    int depth_count, i;
    int *depth_list;

    setlocale(LC_CTYPE,"");

    XrmInitialize();
    
    MAIN_ParseOptions( &argc, argv );

#ifdef MALLOC_DEBUGGING
    if(debugging_malloc)
    {
       char *trace=getenv("MALLOC_TRACE");
       if(!trace)
       {       
       	dprintf_malloc(stddeb,"MALLOC_TRACE not set. No trace generated\n");
       }else
       {
               dprintf_malloc(stddeb,"malloc trace goes to %s\n",trace);
               mtrace();
       }
      mcheck(malloc_error);
    }
#endif

    screen       = DefaultScreenOfDisplay( display );
    screenWidth  = WidthOfScreen( screen );
    screenHeight = HeightOfScreen( screen );
    if (screenDepth)  /* -depth option specified */
    {
	depth_list = XListDepths(display,DefaultScreen(display),&depth_count);
	for (i = 0; i < depth_count; i++)
	    if (depth_list[i] == screenDepth) break;
	XFree( depth_list );
	if (i >= depth_count)
	{
	    fprintf( stderr, "%s: Depth %d not supported on this screen.\n",
		              Options.programName, screenDepth );
	    exit(1);
	}
    }
    else screenDepth  = DefaultDepthOfScreen( screen );
    if (Options.synchronous) XSynchronize( display, True );
    if (Options.desktopGeometry) MAIN_CreateDesktop( argc, argv );
    else rootWindow = DefaultRootWindow( display );

    RELAY_Init();
    MAIN_SaveSetup();
    DOS_InitFS();
    Comm_Init();
#ifndef WINELIB
    INT21_Init();
#endif
#ifndef sparc
    atexit(called_at_exit);
#else
    on_exit (called_at_exit, 0);
#endif

    ret_val = _WinMain( argc, argv );

    return ret_val;
}

/***********************************************************************
 *           MessageBeep    (USER.104)
 */
void MessageBeep(WORD i)
{
	XBell(display, 100);
}

/***********************************************************************
 *      GetVersion (KERNEL.3)
 */
LONG GetVersion(void)
{
	return( 0x03300a03 ); /*  dos 3.30 & win 3.10 */
}

/***********************************************************************
 *	GetWinFlags (KERNEL.132)
 */
LONG GetWinFlags(void)
{
	return (WF_STANDARD | WF_CPU286 | WF_PMODE | WF_80x87);
}

/***********************************************************************
 *	SetEnvironment (GDI.132)
 */
int SetEnvironment(LPSTR lpPortName, LPSTR lpEnviron, WORD nCount)
{
	LPENVENTRY	lpNewEnv;
	LPENVENTRY	lpEnv = lpEnvList;
	printf("SetEnvironnement('%s', '%s', %d) !\n", 
				lpPortName, lpEnviron, nCount);
	if (lpPortName == NULL) return -1;
	while (lpEnv != NULL) {
		if (lpEnv->Name != NULL && strcmp(lpEnv->Name, lpPortName) == 0) {
			if (nCount == 0 || lpEnviron == NULL) {
				if (lpEnv->Prev != NULL) lpEnv->Prev->Next = lpEnv->Next;
				if (lpEnv->Next != NULL) lpEnv->Next->Prev = lpEnv->Prev;
				free(lpEnv->Value);
				free(lpEnv->Name);
				free(lpEnv);
				printf("SetEnvironnement() // entry deleted !\n");
				return -1;
				}
			free(lpEnv->Value);
			lpEnv->Value = malloc(nCount);
			if (lpEnv->Value == NULL) {
				printf("SetEnvironment() // Error allocating entry value !\n");
				return 0;
			}
			memcpy(lpEnv->Value, lpEnviron, nCount);
			lpEnv->wSize = nCount;
			printf("SetEnvironnement() // entry modified !\n");
			return nCount;
			}
		if (lpEnv->Next == NULL) break;
		lpEnv = lpEnv->Next;
		}
	if (nCount == 0 || lpEnviron == NULL) return -1;
	printf("SetEnvironnement() // new entry !\n");
	lpNewEnv = malloc(sizeof(ENVENTRY));
	if (lpNewEnv == NULL) {
		printf("SetEnvironment() // Error allocating new entry !\n");
		return 0;
		}
	if (lpEnvList == NULL) {
		lpEnvList = lpNewEnv;
		lpNewEnv->Prev = NULL;
		}
	else {
		lpEnv->Next = lpNewEnv;
		lpNewEnv->Prev = lpEnv;
		}
	lpNewEnv->Next = NULL;
	lpNewEnv->Name = malloc(strlen(lpPortName) + 1);
	if (lpNewEnv->Name == NULL) {
		printf("SetEnvironment() // Error allocating entry name !\n");
		return 0;
		}
	strcpy(lpNewEnv->Name, lpPortName);
	lpNewEnv->Value = malloc(nCount);
	if (lpNewEnv->Value == NULL) {
		printf("SetEnvironment() // Error allocating entry value !\n");
		return 0;
		}
	memcpy(lpNewEnv->Value, lpEnviron, nCount);
	lpNewEnv->wSize = nCount;
	return nCount;
}

/***********************************************************************
 *	GetEnvironment (GDI.134)
 */
int GetEnvironment(LPSTR lpPortName, LPSTR lpEnviron, WORD nMaxSiz)
{
	WORD		nCount;
	LPENVENTRY	lpEnv = lpEnvList;
	printf("GetEnvironnement('%s', '%s', %d) !\n",
					lpPortName, lpEnviron, nMaxSiz);
	while (lpEnv != NULL) {
		if (lpEnv->Name != NULL && strcmp(lpEnv->Name, lpPortName) == 0) {
			nCount = min(nMaxSiz, lpEnv->wSize);
			memcpy(lpEnviron, lpEnv->Value, nCount);
			printf("GetEnvironnement() // found '%s' !\n", lpEnviron);
			return nCount;
			}
		lpEnv = lpEnv->Next;
		}
	printf("GetEnvironnement() // not found !\n");
	return 0;
}

/***********************************************************************
 *	GetTimerResolution (USER.14)
 */
LONG GetTimerResolution(void)
{
	return (1000);
}

/***********************************************************************
 *	SystemParametersInfo (USER.483)
 */
BOOL SystemParametersInfo (UINT uAction, UINT uParam, void FAR *lpvParam, UINT fuWinIni)
{
	int timeout, temp;
	char buffer[256];
	XKeyboardState		keyboard_state;
	XKeyboardControl	keyboard_value;


	fprintf(stderr, "SystemParametersInfo: action %d, param %x, flag %x\n", 
			uAction, uParam, fuWinIni);

	switch (uAction) {
		case SPI_GETBEEP:
			XGetKeyboardControl(display, &keyboard_state);
			if (keyboard_state.bell_percent == 0)
				*(BOOL *) lpvParam = FALSE;
			else
				*(BOOL *) lpvParam = TRUE;
			break;
		
		case SPI_GETBORDER:
			*(INT *) lpvParam = 1;
			break;

		case SPI_GETFASTTASKSWITCH:
			*(BOOL *) lpvParam = FALSE;
			break;

		case SPI_GETGRIDGRANULARITY:
			*(INT *) lpvParam = 1;
			break;

		case SPI_GETICONTITLEWRAP:
			*(BOOL *) lpvParam = FALSE;
			break;

		case SPI_GETKEYBOARDDELAY:
			*(INT *) lpvParam = 1;
			break;

		case SPI_GETKEYBOARDSPEED:
			*(WORD *) lpvParam = 30;
			break;

		case SPI_GETMENUDROPALIGNMENT:
			*(BOOL *) lpvParam = FALSE;
			break;

		case SPI_GETSCREENSAVEACTIVE:
			*(BOOL *) lpvParam = FALSE;
			break;

		case SPI_GETSCREENSAVETIMEOUT:
			XGetScreenSaver(display, &timeout, &temp,&temp,&temp);
			*(INT *) lpvParam = timeout * 1000;
			break;

		case SPI_ICONHORIZONTALSPACING:
			if (lpvParam == NULL)
				fprintf(stderr, "SystemParametersInfo: Horizontal icon spacing set to %d\n.", uParam);
			else
				*(INT *) lpvParam = 50;
			break;

		case SPI_ICONVERTICALSPACING:
			if (lpvParam == NULL)
				fprintf(stderr, "SystemParametersInfo: Vertical icon spacing set to %d\n.", uParam);
			else
				*(INT *) lpvParam = 50;
			break;

		case SPI_SETBEEP:
			if (uParam == TRUE)
				keyboard_value.bell_percent = -1;
			else
				keyboard_value.bell_percent = 0;			
   			XChangeKeyboardControl(display, KBBellPercent, 
   							&keyboard_value);
			break;

		case SPI_SETSCREENSAVEACTIVE:
			if (uParam == TRUE)
				XActivateScreenSaver(display);
			else
				XResetScreenSaver(display);
			break;

		case SPI_SETSCREENSAVETIMEOUT:
			XSetScreenSaver(display, uParam, 60, DefaultBlanking, 
						DefaultExposures);
			break;

		case SPI_SETDESKWALLPAPER:
			return (SetDeskWallPaper((LPSTR) lpvParam));
			break;

		case SPI_SETDESKPATTERN:
			if ((INT) uParam == -1) {
				GetProfileString("Desktop", "Pattern", 
						"170 85 170 85 170 85 170 85", 
						buffer, sizeof(buffer) );
				return (DESKTOP_SetPattern((LPSTR) buffer));
			} else
				return (DESKTOP_SetPattern((LPSTR) lpvParam));
			break;

		case SPI_LANGDRIVER:
		case SPI_SETBORDER:
		case SPI_SETDOUBLECLKHEIGHT:
		case SPI_SETDOUBLECLICKTIME:
		case SPI_SETDOUBLECLKWIDTH:
		case SPI_SETFASTTASKSWITCH:
		case SPI_SETKEYBOARDDELAY:
		case SPI_SETKEYBOARDSPEED:
			fprintf(stderr, "SystemParametersInfo: option %d ignored.\n", uParam);
			break;

		default:
			fprintf(stderr, "SystemParametersInfo: unknown option %d.\n", uParam);
			break;
	}
	return 1;
}

/***********************************************************************
*	HMEMCPY (KERNEL.348)
*/
void hmemcpy(void FAR *hpvDest, const void FAR *hpvSource, long cbCopy)
{
	memcpy(hpvDest,	hpvSource, cbCopy);
}

/***********************************************************************
*	COPY (GDI.250)
*/
void Copy(LPVOID lpSource, LPVOID lpDest, WORD nBytes)
{
	memcpy(lpDest, lpSource, nBytes);
}

/***********************************************************************
*	SWAPMOUSEBUTTON (USER.186)
*/
BOOL SwapMouseButton(BOOL fSwap)
{
	return 0;	/* don't swap */
}

/***********************************************************************
*	FileCDR (KERNEL.130)
*/
void FileCDR(FARPROC x)
{
	printf("FileCDR(%8x)\n", (int) x);
}

/*
 * Windows widgets (built-in window classes)
 *
 * Copyright 1993 Alexandre Julliard

static char Copyright[] = "Copyright  Alexandre Julliard, 1993";
*/

#include "win.h"
#include "button.h"
#include "static.h"
#include "scroll.h"
#include "desktop.h"
#include "mdi.h"
#include "gdi.h"
#include "user.h"
#include "selectors.h"


static WNDCLASS WIDGETS_BuiltinClasses[] =
{
    { CS_GLOBALCLASS | CS_PARENTDC, (WNDPROC)"ButtonWndProc", 0,
      sizeof(BUTTONINFO), 0, 0, 0, 0, NULL, "BUTTON" },
    { CS_GLOBALCLASS | CS_PARENTDC, (WNDPROC)"StaticWndProc", 0,
      sizeof(STATICINFO), 0, 0, 0, 0, NULL, "STATIC" },
    { CS_GLOBALCLASS | CS_PARENTDC, (WNDPROC)"ScrollBarWndProc", 0,
      sizeof(SCROLLINFO), 0, 0, 0, 0, NULL, "SCROLLBAR" },
    { CS_GLOBALCLASS | CS_PARENTDC | CS_DBLCLKS, (WNDPROC)"ListBoxWndProc", 0,
      8, 0, 0, 0, 0, NULL, "LISTBOX" },
    { CS_GLOBALCLASS | CS_PARENTDC | CS_DBLCLKS, (WNDPROC)"ComboBoxWndProc", 0,
      8, 0, 0, 0, 0, NULL, "COMBOBOX" },
    { CS_GLOBALCLASS, (WNDPROC)"EditWndProc", 0,
      sizeof(WORD), 0, 0, 0, 0, NULL, "EDIT" },
    { CS_GLOBALCLASS | CS_SAVEBITS, (WNDPROC)"PopupMenuWndProc", 0,
      8, 0, 0, 0, 0, NULL, POPUPMENU_CLASS_NAME },
    { CS_GLOBALCLASS, (WNDPROC)"DesktopWndProc", 0,
      sizeof(DESKTOPINFO), 0, 0, 0, 0, NULL, DESKTOP_CLASS_NAME },
    { CS_GLOBALCLASS | CS_SAVEBITS, (WNDPROC)"DefDlgProc", 0,
      DLGWINDOWEXTRA, 0, 0, 0, 0, NULL, DIALOG_CLASS_NAME },
    { CS_GLOBALCLASS, (WNDPROC)"MDIClientWndProc", 0,
      sizeof(MDICLIENTINFO), 0, 0, 0, STOCK_LTGRAY_BRUSH, NULL, "MDICLIENT" }
};

#define NB_BUILTIN_CLASSES \
         (sizeof(WIDGETS_BuiltinClasses)/sizeof(WIDGETS_BuiltinClasses[0]))


/***********************************************************************
 *           WIDGETS_Init
 * 
 * Initialize the built-in window classes.
 */
BOOL WIDGETS_Init(void)
{
    int i;
    HANDLE hName;
    char *name;
    WNDCLASS *class = WIDGETS_BuiltinClasses;

    if (!(hName = USER_HEAP_ALLOC( 20 ))) return FALSE;
    name = USER_HEAP_LIN_ADDR( hName );
    for (i = 0; i < NB_BUILTIN_CLASSES; i++, class++)
    {
        strcpy( name, class->lpszClassName );
        class->lpszClassName = (LPSTR)USER_HEAP_SEG_ADDR( hName );
	class->hCursor = LoadCursor( 0, IDC_ARROW );
        class->lpfnWndProc = GetWndProcEntry16( (char *)class->lpfnWndProc );
	if (!RegisterClass( class )) return FALSE;
    }
    USER_HEAP_FREE( hName );
    return TRUE;
}

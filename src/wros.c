/*************************************************
*       The E text editor - 2nd incarnation      *
*************************************************/

/* Copyright (c) University of Cambridge, 1991 - 1999 */

/* Written by Philip Hazel, starting November 1991 */
/* This file last modified: November 1999 */


/* This file contains the code for running E in a RISC OS window */


#include "ehdr.h"
#include "keyhdr.h"
#include "cmdhdr.h"
#include "shdr.h"
#include "roshdr.h"
#include "scomhdr.h"


#define osbyte(a,b,c) _kernel_osbyte(a,b,c)

/* Info box field for the version string */

#define info_field 4

#define ftype_text 0xFFF

#define menu_item_height 44

#define wimp_window_width 256
#define window_title_len 150


/* Indicator columns in window_title */

#define wt_stream               0
#define wt_overstrike           1
#define wt_append               2
#define wt_casematch            3
#define wt_autoalign            4
#define wt_margin               5
#define wt_mark                 7
#define wt_name                 9




/*************************************************
*              Static variables                  *
*************************************************/

#ifdef SCRNDEBUG
FILE *scrndebug;
#endif


/* Wimp messages that are important to this task. Note that Message_Quit
is zero, so don't include it in the list, because it will terminate it! */

static int wimp_message_list[] = { Message_PreQuit, Message_DataOpen, Message_DataLoad,
  Message_DataSave, Message_DataSaveAck, Message_TaskWindow_Output,
  Message_TaskWindow_Ego, Message_TaskWindow_Morio, Message_TaskWindow_NewTask, 0 };

static int application_done = FALSE;
static int background_colour = 1;
static int colour_background = 1;
static int colour_cursor = 14;
static int colour_mark = 15;
static int colour_mark_and_cursor = 11;
static int menu_handle = -1;
static int old_linecount = -1;
static bufferstr *save_buffer;
static int shown_key = -1;
static int shown_col = -1;
static int text_written = FALSE;
static int tw_change = FALSE;
static int use_dragasprite;

static int poll_mask = M_All - M_Key_Pressed - M_Mouse_Click - M_Redraw_Window_Request -
  M_User_Message - M_User_Message_Recorded - M_Gain_Caret - M_Lose_Caret;

static int handle_info;
static window_data *window_info;

static int handle_text = -1;
static window_data *window_text;

static int handle_etext = -1;
static window_data *window_etext;

static int handle_cmdline = -1;
static window_data *window_cmdline;

static int handle_query;
static window_data *window_query;

static int handle_closequery;
static window_data *window_closequery;

static int handle_save;
static window_data *window_save;

static int handle_position;
static window_data *window_position;


/*************************************************
*                 Static Menus                   *
*************************************************/

/* The menu block defines just one menu item, as C can't
have a vector of undefined length in a structure, even at the
end. Therefore, subsequent blocks must be tacked on immediately
afterwards. */


/******** Icon bar menu ********/

static menu_block menu_iconbar = {
  "NE",
  7, 2,                /* title colours */
  7, 0,                /* work area colours */
  150,                 /* item width */
  menu_item_height,
  0,                   /* gap */
    {
    0,
    (menu_block *)(-1),
    F_text + F_filled + 0x07000000,
    "Info"
    }
};

static menu_item menu_iconbar_rest[] = {
  {
  0,
  (menu_block *)(-1),
  F_text + F_filled + 0x07000000,
  "Task"
  },
  {
  0,
  (menu_block *)(-1),
  F_text + F_filled + 0x07000000,
  "Line info"
  },
  {
  F_last,
  (menu_block *)(-1),
  F_text + F_filled + 0x07000000,
  "Quit"
  }
};

enum { iconbar_info, iconbar_taskwindow, iconbar_position, iconbar_quit };



/******** Text window menu ********/

static menu_block menu_textwindow = {
  "NE",
  7, 2,                /* title colours */
  7, 0,                /* work area colours */
  100,                 /* item width */
  menu_item_height,
  0,                   /* gap */
    {
    F_last,            /* this is normally last, but gets overridden for */
    (menu_block *)(-1),    /* task windows */
    F_text + F_filled + 0x07000000,
    "save"
    }
};

static menu_item menu_textwindow_rest[] = {
    {
    F_last,
    (menu_block *)(-1),
    F_text + F_filled + 0x07000000,
    "kill task"
    }
};

enum { textmenu_save, textmenu_killtask };



/*************************************************
*                Global variables                *
*************************************************/

int mode_max_col;
int mode_max_row;

BOOL support_taskwindows = TRUE;


/*************************************************
*              Screen debug routine              *
*************************************************/

#ifdef SCRNDEBUG
static void scrndebug_printf(char *format, ...)
{
va_list ap;
va_start(ap, format);
vfprintf(scrndebug, format, ap);
va_end(ap);
}
#endif



/*************************************************
*                Kill task                       *
*************************************************/

/* It's the task associated with the current window
that's killed. */

static void control_task(int action)
{
msg_data_block md;
md.length = 20;
md.action = action;
SWI(Wimp_SendMessage, 3, User_Message, (int)(&md), currentbuffer->windowhandle2);
}


/*************************************************
*           Get "in front" handle                *
*************************************************/

/* If the position window is open, put *it* in front,
and the requested window behind it. */

static int pos_in_front(void)
{
if (currentbuffer != NULL && (menu_iconbar.items[iconbar_position].flags & F_tick) != 0)
  {
  window_state_block ws;
  ws.handle = handle_position;
  SWI(Wimp_GetWindowState, 2, 0, (int)(&ws));
  if ((ws.flags & 0x10000) != 0)
    {
    if (ws.infront != -1)
      {
      ws.infront = -1; 
      SWI(Wimp_OpenWindow, 2, 0, (int)(&ws));
      } 
    return handle_position;   
    } 
  } 
return -1;
}


/*************************************************
*               Open a window                    *
*************************************************/

/* The window is opened right at the front if infront = TRUE; otherwise
it is opened behind the position window, if that exists. */

static void open_window(int handle, BOOL infront, BOOL zeroscroll)
{
window_state_block ws;
ws.handle = handle;
SWI(Wimp_GetWindowState, 2, 0, (int)(&ws));
ws.infront = (infront)? -1 : pos_in_front();
if (zeroscroll) ws.scrollx = ws.scrolly = 0;
SWI(Wimp_OpenWindow, 2, 0, (int)(&ws));
}


/*************************************************
*        Put given line at window top            *
*************************************************/

void sys_window_topline(int number)
{
window_state_block ws;
ws.handle = handle_etext;
SWI(Wimp_GetWindowState, 2, 0, (int)(&ws));
ws.scrolly = -number*char_height;
SWI(Wimp_OpenWindow, 2, 0, (int)(&ws));
}


/*************************************************
*         Display text in scrolling window       *
*************************************************/

void sys_window_display_text(char *s)
{
if (handle_text < 0)
   handle_text = scrw_init(window_text, 80, 4096, 4096, scrw_malloc_only);
scrw_puts(handle_text, s);
text_written = TRUE;
}



/*************************************************
*            Find line by logical number         *
*************************************************/

/* We assume we are near the current line, so start from there
to try to save time. */

static linestr *find_line(int *number)
{
int n = *number;
linestr *line = main_current;
int linenumber = main_currentlinenumber;

while (n < linenumber)
  {
  if (line->prev == NULL) break;
  line = line->prev;
  linenumber--;
  }

while (n > linenumber)
  {
  if (line->next == NULL) break;
  line = line->next;
  linenumber++;
  }

*number = linenumber;
return line;
}



/*************************************************
*            Find character pointer              *
*************************************************/

/* This function finds a character pointer, given a row and column
in the virtual window that is formed by the workspace. The count
of available characters (which can be zero) is also returned. */

/* The current line number, counting from 1, tells us the relative
number of the current line in the file. Search around from there to
find the required line. */

static char *wros_find_cp(int row, int col, int *count)
{
char *cp;
linestr *line = main_current;
int linenumber = main_currentlinenumber;

while (row < linenumber)
  {
  if (line->prev == NULL) break;
  line = line->prev;
  linenumber--;
  }

while (row > linenumber)
  {
  if (line->next == NULL) break;
  line = line->next;
  linenumber++;
  }

cp = line->text + col;
*count = line->len - col;
return cp;
}




/*************************************************
*             Redraw rectangles                  *
*************************************************/

/* This function is called by wros_redraw_window() and wros_update_window()
to go through the rectangles and do the necessary. It need only clear the
rectangles if the flag is set. */

static void wros_redraw_rectangles(int more, window_redraw_block *r, int clear)
{
char buff[256];

while (more)
  {
  int y;
  int minx = r->gminx - (r->minx - r->scrollx);      /* Calculate the rectangle to be */
  int maxx = r->gmaxx - (r->minx - r->scrollx);      /* updated in workarea coordinates. */
  int miny = r->gminy - (r->maxy - r->scrolly);
  int maxy = r->gmaxy - (r->maxy - r->scrolly);

  if (maxx > wimp_window_width * char_width + 4) maxx = wimp_window_width * char_width;

  /* Loop for all the character rows that need updating, starting from the
  one that sticks down into the top of the rectangle. The variables x and
  y are in workarea coordinates. */

  for (y = maxy + abs(maxy)%char_height; y > miny; y -= char_height)
    {
    char *p = buff;
    int x = (minx-4) - (minx-4)%char_width;

    /* Calculate the screen coordinates of the top left-hand corner of the
    first character in the row. */

    int xa = x + (r->minx - r->scrollx);
    int ya = y + (r->maxy - r->scrolly);

    /* Calculate the row and column within the virtual window */

    int row = abs(y)/char_height;
    int col = x/char_width;

    /* Find the pointer to the first character and the count left */

    int count;
    char *cp = wros_find_cp(row, col, &count);

    /* Clear the rectangle to the background colour if necessary. The flag is set
    after user-initiated re-draws, not after wimp-initiated re-draws. This clearing
    may be a waste if an entire line is in inverse video, but it is simpler to
    operate this way. */

    if (clear)
      {
      SWI(Wimp_SetColour, 1, background_colour + 0x80);
      *p++ = 25;
      *p++ = 4;
      *p++ = r->gminx;
      *p++ = r->gminx >> 8;
      *p++ = ya;
      *p++ = ya >> 8;
      *p++ = 25;
      *p++ = 103;
      *p++ = r->gmaxx;
      *p++ = r->gmaxx >> 8;
      *p++ = (ya - char_height);
      *p++ = (ya - char_height) >> 8;
      }

    /* Move the current graphics position to the top left character place. We
    adjust all y values by -4 so that the top character in the window doesn't
    get its top pixels chopped off. We adjust all x values by 4 to make a small
    indent. */

    *p++ = 25;
    *p++ = 4;
    *p++ = xa + 4;
    *p++ = (xa + 4) >> 8;
    *p++ = ya - 4;
    *p++ = (ya - 4) >> 8;

    /* Add the characters to the VDU buffer, and update the x positions, but
    not if we are at the end of the row's data. */

    while (x < maxx)
      {
      if (count-- > 0)
        {
        int ch = *cp++;
        *p++ = (ch < 32 || 
          (ch >= 127 && ch < topbit_minimum) || (!main_eightbit && ch >= topbit_minimum))? 
            '?' : ch;
        }
      x += char_width;
      xa += char_width;
      }

    /* Output remaining queued characters, if any. */

    if (p > buff) SWI(OS_WriteN, 2, buff, p - buff);
    }

  /* Try for more rectangles */

  more = SWI(Wimp_GetRectangle, 2, 0, (int)r);
  }
}



/*************************************************
*             Update a window                    *
*************************************************/

/* This function is called when we want to update part of a
window. The rectangle is given in work area coordinates. */

static void wros_update_window(int minx, int miny, int maxx, int maxy)
{
window_redraw_block r;
r.handle = handle_etext;
r.minx = minx + 4;
r.miny = miny;
r.maxx = maxx + 4;
r.maxy = maxy;
wros_redraw_rectangles(SWI(Wimp_UpdateWindow, 2, 0, (int)(&r)), &r, TRUE);
}


/*************************************************
*        Refresh the window at leisure           *
*************************************************/

/* Do nothing if no window current (can happen during close down
or deleting the only buffer). */

void sys_window_reshow(void)
{
window_state_block ws;
if (handle_etext < 0) return;
ws.handle = handle_etext;
SWI(Wimp_GetWindowState, 2, 0, (int)(&ws));
wros_update_window(ws.scrollx, ws.scrolly - (ws.maxy - ws.miny),
  ws.scrollx + ws.maxx - ws.minx, ws.scrolly);
}


/*************************************************
*         Refresh the window immediately         *
*************************************************/

/* Can we do this? */

void sys_window_refresh(void)
{
}



/*************************************************
*        Show or unshow the cursor               *
*************************************************/

/* Also handles the mark when set */

void sys_display_cursor(int show)
{
background_colour = show? colour_cursor : colour_background;

wros_update_window(cursor_col*char_width, -(main_currentlinenumber+1)*char_height,
  (cursor_col+1)*char_width, 4 - (main_currentlinenumber*char_height));

if (mark_line != NULL)
  {
  int marklinenumber = 0;
  linestr *line = main_top;
  while (line != NULL && line != mark_line)
    {
    line = line->next;
    marklinenumber++;
    }

  background_colour = show?
    ((mark_col == cursor_col && marklinenumber == main_currentlinenumber)?
      colour_mark_and_cursor : colour_mark) : colour_background;

  wros_update_window(mark_col*char_width, -(marklinenumber+1)*char_height,
    (mark_col+1)*char_width, 4 - (marklinenumber*char_height));
  }

background_colour = colour_background;
currentbuffer->cursor_shown = show;
}


/*************************************************
*             Redraw a window                    *
*************************************************/

/* This function is called in response to a Redraw_Window_Request
return from Wimp_Poll. */

static void wros_redraw_window(void)
{
window_redraw_block r;
r.handle = handle_etext;
wros_redraw_rectangles(SWI(Wimp_RedrawWindow, 2, 0, (int)(&r)), &r, FALSE);
if (currentbuffer->cursor_shown) sys_display_cursor(TRUE);
}



/*************************************************
*          Set size of work area                 *
*************************************************/

/* The vertical size of the work area should reflect the number of lines
being edited. A minimum of 30 lines is imposed. The horizontal extent is
set arbitrarily at 256 characters. For large files, there appears to be
a rounding error problem that makes it impossible to scroll to the bottom
by dragging (though one can always scroll by pressing the scroll down
icon). There seems no way round this. */

static void set_extent(void)
{
if (main_linecount != old_linecount)
  {
  int n = main_linecount;
  int block[4];
  if (n < 30) n = 30;
  block[0] = 0;
  block[1] = -((n + 1) * char_height + 4);
  block[2] = wimp_window_width * char_width + 8;
  block[3] = 0;
  SWI(Wimp_SetExtent, 2, handle_etext, (int)block);
  old_linecount = main_linecount;
  }
}


/*************************************************
*        Ensure current char is in window        *
*************************************************/

/* We don't count the top or bottom lines, as they are often only half
visible. */

static void align_window(window_state_block *ws, linestr *topline,
  linestr *botline, int depth, BOOL infront, BOOL realbottom)
{
int number;
int width = ws->maxx - ws->minx;
int x = cursor_col * char_width + 4;
BOOL xright, xleft, xvis, yvis;
linestr *line = topline;

/* Determine if horizontal visibility is OK */

xleft = x < ws->scrollx;
xright = x >= ws->scrollx + ws->maxx - ws->minx - char_width;
xvis = !xleft && !xright;

/* Determine if vertical visibility is OK */

yvis = FALSE;
for (;;)
  {
  if (line == main_current)
    {
    if (line != topline && (line != botline || !realbottom)) yvis = TRUE;
    break;
    }
  if (line == botline) break;
  line = line->next;
  }

/* If we have either horizontal or vertical non-visibility, we
re-open the window with changed scroll values. Otherwise return
FALSE, to indicate nothing done. */

if (yvis && xvis) return;

/* The current line is not visible. If it is only one away from the top
or bottom, try to scroll the screen by just one line. Otherwise plunk
the current line in the middle. */

if (!yvis)
  {
  line = main_current;
  number = main_currentlinenumber;

  if (line == topline || line->next == topline) depth = 0;
    else if (line != botline) depth /= 2; else depth--;

  while (depth-- > 0)
    {
    if (line->prev == NULL) break;
    line = line->prev;
    number--;
    }
  }

if (!xvis) ws->scrollx = xright? cursor_col*char_width - width/2 :
  cursor_col*char_width - (3*width)/4;

if (!yvis)
  {
  int newscrolly = -(number * char_height);
  if (newscrolly != ws->scrolly) cmd_saveback();
  ws->scrolly = newscrolly;
  }

if (infront) ws->infront = pos_in_front();
SWI(Wimp_OpenWindow, 2, 0, (int)ws);
}


/*************************************************
*         Cursor top left or bottom right        *
*************************************************/

/* These keystrokes have to be handled here. */

void sys_window_diag(BOOL topleft)
{
int n;
window_state_block ws;
ws.handle = handle_etext;
SWI(Wimp_GetWindowState, 2, 0, (int)(&ws));

n = -ws.scrolly/char_height;
cursor_col = (ws.scrollx + char_width/2 - 4)/char_width;

if (!topleft)
  {
  cursor_col += (ws.maxx - ws.minx)/char_width - 2;
  n += (ws.maxy - ws.miny)/char_height;
  }

main_current = find_line(&n);
main_currentlinenumber = n;
}


/*************************************************
*            Scroll text in a window             *
*************************************************/

/* This function is called when one of the four scroll
keystrokes is pressed. All we need to is to arrange adjust
the current line or the cursor column; the automatic code
that ensures the cursor is seen will do the rest. */

void sys_window_scroll(int type)
{
int width, height;
window_state_block ws;
ws.handle = handle_etext;
SWI(Wimp_GetWindowState, 2, 0, (int)(&ws));

width  = (ws.maxx - ws.minx)/char_width;
height = (ws.maxy - ws.miny)/char_height;

/* Handle vertical scrolls */

if (type <= wscroll_down)
  {
  int newlinenumber = (-ws.scrolly/char_height) +
    ((type == wscroll_up)? (-height/2) : (3*height)/2);

  while (newlinenumber > main_currentlinenumber)
    {
    if ((main_current->flags & lf_eof) != 0) break;
    main_current = main_current->next;
    main_currentlinenumber++;
    }

  while (newlinenumber < main_currentlinenumber)
    {
    if (main_current->prev == NULL) break;
    main_current = main_current->prev;
    main_currentlinenumber--;
    }
  }

/* Handle horizontal scrolls */

else
  {
  cursor_col = ws.scrollx/char_width +
    ((type == wscroll_right)? width + 1 : -width/2);
  if (cursor_col < 0) cursor_col = 0;
  }
}



/*************************************************
*      Update after keystroke or command         *
*************************************************/

/* This function is called from the main scrn_display() function. Its
job is to get the window up-to-date after possible changes. This must
be done by finding out which lines are displayed, and marking invalid
those parts of the window which contain any lines that are flagged for
re-display. This is also called after every keystroke. */

void sys_window_display(BOOL align, BOOL infront)
{
int top, bot, y;
linestr *line, *topline, *botline;
window_state_block ws;

if (handle_etext < 0) return;

ws.handle = handle_etext;
SWI(Wimp_GetWindowState, 2, 0, (int)(&ws));

top = 0 - ws.scrolly/char_height;
bot = top + (ws.maxy - ws.miny)/char_height;
topline = find_line(&top);
botline = find_line(&bot);

set_extent();

if (align) align_window(&ws, topline, botline, bot - top, infront, 
  (bot == top + (ws.maxy - ws.miny)/char_height));

line = topline;
y = ws.scrolly - ws.scrolly % char_height;

for (;;)
  {
  if ((line->flags & (lf_shn | lf_clend)) != 0)
    {
    wros_update_window(ws.scrollx, y - char_height, ws.scrollx + ws.maxx - ws.minx, y);
    line->flags &= ~(lf_shn | lf_clend);
    }
  if (line == botline) break;
  line = line->next;
  y -= char_height;
  }
}



/*************************************************
*            Cause a line to be reshown          *
*************************************************/

/* The row number is the absolute line number. */

void sys_window_display_line(int row, int col)
{
wros_update_window(col*char_width, -(row+1)*char_height,
  wimp_window_width*char_width, -row*char_height);
}



/*************************************************
*          Display the line number               *
*************************************************/

/* Not used in this windowing version */

void sys_window_display_linenumber(char *s)
{
}



/*************************************************
*                 Delete lines                   *
*************************************************/

/* Called when one or more lines are deleted. We must scroll up
the workspace, and reduce its size. Main_linecount will have
already been set to the new size. */

void sys_window_deletelines(int row, int count)
{
int ybot = -(main_linecount + count)*char_height;
int ytop = ybot + count*char_height;
int xright = wimp_window_width*char_width;
SWI(Wimp_BlockCopy, 7, handle_etext, 0, ybot, xright,
  -(row + count)*char_height, 0, ytop);
wros_update_window(0, ybot, xright, ytop);
set_extent();
}




/*************************************************
*                 Insert lines                   *
*************************************************/

/* Called when one or more lines are inserted. We must increase the
size of the workspace, and then scroll it up. The value of main_linecount
has been increased before calling this function. The row gives the
line before which the insertions happen. */

void sys_window_insertlines(int row, int count)
{
int ybot = -(main_linecount - count)*char_height;

set_extent();

SWI(Wimp_BlockCopy, 7, handle_etext, 0, ybot, wimp_window_width*char_width,
  -row*char_height, 0, ybot - count*char_height);
}



/*************************************************
*             Give window information            *
*************************************************/

/* Pass back information about the window size and scroll position. */

void sys_window_info(int *data)
{
window_state_block ws;
ws.handle = handle_etext;
SWI(Wimp_GetWindowState, 2, 0, (int)(&ws));

data[0] = ws.scrollx/char_width;
data[1] = 1 + (ws.maxy - ws.miny - ws.scrolly)/char_height;
data[2] = (ws.scrollx + ws.maxx - ws.minx)/char_width;
data[3] = 1 - ws.scrolly/char_height;
}




/*************************************************
*           Select buffer                        *
*************************************************/

static void selectbuffer(bufferstr *buffer)
{
init_selectbuffer(buffer, FALSE);
handle_etext = buffer->windowhandle;
}



/*************************************************
*            Find a buffer from a handle         *
*************************************************/

/* This also selects the buffer. It should always succeed,
since we should only get events applying to our own windows. */

static bufferstr *findbuffer(int handle)
{
bufferstr *buffer = main_bufferchain;
while (buffer != NULL)
  {
  if (handle == buffer->windowhandle)
    {
    selectbuffer(buffer);
    return buffer;
    }
  buffer = buffer->next;
  }
return NULL;
}



/*************************************************
*        Find a buffer from a task handle        *
*************************************************/

/* This also selects the buffer. It should always succeed,
since we should only get events applying to our own task windows. */

static bufferstr *findtaskbuffer(int task)
{
bufferstr *buffer = main_bufferchain;
while (buffer != NULL)
  {
  if (task == buffer->windowhandle2)
    {
    selectbuffer(buffer);
    return buffer;
    }
  buffer = buffer->next;
  }
return NULL;
}



/*************************************************
*           Get a command line                   *
*************************************************/

/* This is called while obeying commands or keystrokes, and
so the cursor isn't visible on entry. */

void sys_window_cmdline(char *title)
{
int sp = cmd_stackptr;
int pollreturn[64];
char *cmd;
char *cmdbuff = cmd_buffer;
caret_position cp;
S_Key_Pressed *sk = (S_Key_Pressed *)pollreturn;

SWI(Wimp_GetCaretPosition, 2, 0, (int)(&cp));

set_icon_text(handle_cmdline, icon_cmdline, "");
if (strlen(title) > 200) title += strlen(title) - 200;
strcpy(window_cmdline->title_data.ind.text, title);
open_window(handle_cmdline, TRUE, FALSE); 
SWI(Wimp_SetCaretPosition, 6, handle_cmdline, icon_cmdline, 0, 0, -1, 0);

/* Show where the cursor is */

sys_display_cursor(TRUE);

/* Turn the hourglass off during keyboard entry, and also turn
off the escape recognition, which will be on because this is
called after a keystroke. */

osbyte(229, 1, 0);
osbyte(124, 0, 0);
if (hourglass_on) SWI(Hourglass_Off, 0);

if (sp == 0) sp = -1;
for (;;)
  {
  BOOL replace = FALSE;
  int pollaction = SWI(Wimp_Poll, 4, M_All - M_Key_Pressed - M_Redraw_Window_Request,
    (int)pollreturn, 0, 0);
     
  switch(pollaction)
    {
    case Open_Window_Request:
    SWI(Wimp_OpenWindow, 2, 0, (int)pollreturn);
    break;

    case Redraw_Window_Request:
    if (pollreturn[0] == handle_text) scrw_poll(pollaction, pollreturn); else
      {
      bufferstr *oldbuffer = currentbuffer;
      bufferstr *buffer = findbuffer(pollreturn[0]);
      wros_redraw_window();
      if (buffer != oldbuffer) selectbuffer(oldbuffer);
      }
    break;

    case Key_Pressed:
    if (sk->handle != handle_cmdline) break;
    if (sk->key == '\r') goto GOTLINE;
    if (sk->key == 0x18F)                      /* up */
      {
      if (sp == 0) sp = cmd_stackptr;
      if (sp > 0) { sp--; replace = TRUE; }
      }
    else if (sk->key == 0x18E)                 /* down */
      {
      if (sp >= 0)
        {
        if (++sp >= cmd_stackptr) sp = 0;
        replace = TRUE;
        }
      }

    if (replace)
      {
      set_icon_text(handle_cmdline, icon_cmdline, cmd_stack[sp]);
      SWI(Wimp_SetCaretPosition, 6, handle_cmdline, icon_cmdline, 0, 0, -1,
        strlen(cmd_stack[sp]));
      }
    break;
    }
  }

GOTLINE:

/* Put back the hourglass and escape recognition, and turn off the
cursor again. */

if (hourglass_on) SWI(Hourglass_On, 0);
osbyte(229, 0, 0);
sys_display_cursor(FALSE);

/* Close the input window, restore the input focus, get the data,
and return. */

SWI(Wimp_CloseWindow, 2, 0, &handle_cmdline);
SWI(Wimp_SetCaretPosition, 6, cp.window, cp.icon, cp.xcaret, cp.ycaret,
  cp.caretflags, cp.caretindex);

cmd = get_icon_text(handle_cmdline, icon_cmdline);
while (*cmd >= 32) *cmdbuff++ = *cmd++;
*cmdbuff = 0;
}



/*************************************************
*      Update position display if visible        *
*************************************************/

static void update_position(BOOL force)
{
if (currentbuffer != NULL && (menu_iconbar.items[iconbar_position].flags & F_tick) != 0)
  {
  int key = main_current->key; 
  char buff[20];
  if (key != shown_key)
    { 
    if (key > 0) sprintf(buff, "%d", key);
      else strcpy(buff, "****");   
    set_icon_text(handle_position, icon_line, buff);
    shown_key = key;
    }  
 
 if (cursor_col != shown_col)
    { 
    sprintf(buff, "%d", cursor_col+1); 
    set_icon_text(handle_position, icon_column, buff); 
    shown_col = cursor_col;
    }  
    
  /* Force an open if requested */
    
  if (force) open_window(handle_position, TRUE, TRUE); 
  }
}   


/*************************************************
*        Flip showing of position display        *
*************************************************/

static void flip_position(void)
{
menu_iconbar.items[iconbar_position].flags ^= F_tick; 
if ((menu_iconbar.items[iconbar_position].flags & F_tick) == 0)
  SWI(Wimp_CloseWindow, 2, 0, &handle_position);
else update_position(TRUE); 
}
   

/*************************************************
*          Indicate status of the mark           *
*************************************************/

/* This routine is called to indicate the status of the mark;
we put something into the title line. It also does the status
of the other flags. Should really change its name... */

void sys_window_showmark(int mark, BOOL markhold)
{
char *pname = main_filealias;
char window_title[window_title_len];

if (currentbuffer == NULL || handle_etext < 0) return;

if (main_filealias != NULL && strlen(main_filealias) > 140 - wt_name)
  pname += strlen(main_filealias) - 140 + wt_name;

strcpy(window_title, "            ");
strcpy(window_title + wt_name, (main_filealias == NULL)? "" : pname);
if (main_filechanged && !tw_change) strcat(window_title + wt_name, " *");

window_title[wt_stream] = (currentbuffer->to_fid == NULL)? ' ' : 'S';
window_title[wt_overstrike] = main_readonly? 'R' : main_overstrike? 'O' : 'I';
window_title[wt_append]  = main_appendswitch?  'A' : 'R';
window_title[wt_casematch] = cmd_casematch?    'V' : 'U';
window_title[wt_autoalign] = main_AutoAlign?   'A' : ' ';
window_title[wt_margin] = (main_rmargin < MAX_RMARGIN)? 'm' : ' ';

window_title[wt_mark] = (mark == mark_unset)? ' ' :
                        (mark == mark_lines)? (markhold? 'B' : 'b') :
                        (mark == mark_text)? 't' :
                        (mark == mark_rect)? 'r' : 'g';

if (strcmp(currentbuffer->windowtitle, window_title) != 0)
  {
  window_state_block ws;
  strcpy(currentbuffer->windowtitle, window_title);
  ws.handle = handle_etext;
  SWI(Wimp_GetWindowState, 2, 0, (int)(&ws));
  SWI(Wimp_ForceRedraw, 5, -1, ws.minx, ws.maxy, ws.maxx, ws.maxy + char_height);
  }
}


/**************************************************
*        Select buffer by command                 *
**************************************************/

void sys_window_selectbuffer(bufferstr *buffer)
{
selectbuffer(buffer);
open_window(handle_etext, FALSE, FALSE);
SWI(Wimp_SetCaretPosition, 6, handle_etext, -1, -100, -100, 12, -1);
}



/**************************************************
*           Handle user query                     *
**************************************************/

/* Returns TRUE if ok to carry on. */

BOOL sys_window_yesno(BOOL savebutton, char *continue_text, char *prompt, ...)
{
int eig, pixels;
int handle = savebutton? handle_closequery : handle_query;
int pollreturn[64];
short int mouseblock[5];
char *buff = (char *)pollreturn;
S_Mouse_Click *sm = (S_Mouse_Click *)pollreturn;

va_list ap;
va_start(ap, prompt);
vsprintf(buff, prompt, ap);
va_end(ap);

if (hourglass_on) SWI(Hourglass_Off, 0);

set_icon_text(handle, icon_query, buff);
set_icon_text(handle, icon_discard, continue_text);
open_window(handle, TRUE, FALSE);

pollreturn[0] = handle;
SWI(Wimp_GetWindowOutline, 2, 0, (int)pollreturn);

/* If we include the window's title in the constraint, a click in it
allows the cursor to escape. */

mouseblock[0] = 0x0100;
mouseblock[1] = pollreturn[1];
mouseblock[2] = pollreturn[2];
mouseblock[3] = pollreturn[3];
mouseblock[4] = pollreturn[4] - 60;

SWI(OS_Word, 2, 21, (int)((char *)mouseblock + 1));

while (SWI(Wimp_Poll, 4, M_All - M_Mouse_Click, (int)pollreturn, 0, 0) != Mouse_Click ||
  sm->handle != handle || sm->icon < 0);

mouseblock[1] = 0;
mouseblock[2] = 0;

SWI(OS_ReadModeVariable, 2, -1, 4);
eig = swi_regs.r[2];
SWI(OS_ReadModeVariable, 2, -1, 11);
pixels = swi_regs.r[2];
mouseblock[3] =  pixels << eig;

SWI(OS_ReadModeVariable, 2, -1, 5);
eig = swi_regs.r[2];
SWI(OS_ReadModeVariable, 2, -1, 12);
pixels = swi_regs.r[2];
mouseblock[4] = pixels << eig;

SWI(OS_Word, 2, 21, (int)((char *)mouseblock + 1));
SWI(Wimp_CloseWindow, 2, 0, &handle);
if (hourglass_on) SWI(Hourglass_On, 0);

if (savebutton && sm->icon == icon_save) return -1;
  else return sm->icon == icon_discard;
}



/*************************************************
*               Save a file                      *
*************************************************/

/* The name can sometimes be terminated by a carriage return. So
copy it to be safe. */

static int save_file(char *name)
{
int yield;
cmdstr cmd;
cmdarg arg;
stringstr string;
bufferstr *oldbuffer = currentbuffer;
char cleanname[100];
char *p = cleanname;

while (*name >= ' ') *p++ = *name++;
*p = 0;

string.text = cleanname;
arg.string = &string;
cmd.arg1 = arg;
cmd.flags = cmdf_arg1;
cmd.misc = (strcmp(cleanname, "<Wimp$Scrap>") == 0)? save_keepname : 0;

if (save_buffer != currentbuffer) selectbuffer(save_buffer);
yield = e_save(&cmd);
if (oldbuffer != currentbuffer) selectbuffer(oldbuffer);

SWI(Wimp_CreateMenu, 4, 0, -1, 0, 0);
SWI(Wimp_CloseWindow, 2, 0, &handle_save);
set_icon_text(handle_save, icon_savename, cleanname);
sys_window_showmark(mark_type, mark_hold);
return yield;
}


/*************************************************
*         Check for file changed                 *
*************************************************/

/* This function is called when a new file is to be loaded;
also when a window is to be closed or the application is to
be shut down. It must ensure that an old buffer isn't lost
by accident. If the user permits discarding, flag the buffer
not changed as some subsequent calls (e.g. dbuffer) also do
the check.

If the user selects "save" from the dialogue box, we can do the
save directly if a suitable name exists. Otherwise, put up the
save box and return FALSE.

Task windows are handled specially, as they don't have the
modified flag set, but what is important is that they are not
closed while the task is still active. The close_window function
has an appropriate check... */

static BOOL allow_new(void)
{
if (currentbuffer->windowhandle2 != 0) return TRUE; else
  {
  int yield = main_filechanged?
    sys_window_yesno(TRUE, "Discard", "%s has been modified",
      (main_filealias == NULL)? "<no name>" : main_filealias) : TRUE;

  /* Deal with "save" - just do it if a fully-qualified name exists */
  
  if (yield < 0)
    {
    save_buffer = currentbuffer; 
    if (main_filealias != NULL && 
       (strstr(main_filealias, "$.") != NULL || 
        strstr(main_filealias, "<") != NULL)) 
      {
      yield = (save_file(main_filealias) == done_continue);
      }
    else
      {
      char buff[20];
      char *typename;
      sprintf(buff, "File$Type_%03X", currentbuffer->filetype);
      typename = getenv(buff);
      if (typename == NULL) typename = buff + 10; 
      set_icon_text(handle_save, icon_savetype, typename);  
       
      set_icon_text(handle_save, icon_savename,
        (main_filealias == NULL || main_filealias[0] == 0)? "TextFile" : main_filealias);
      open_window(handle_save, TRUE, TRUE);
      yield = FALSE; 
      }     
    }       
    
  /* Deal with "discard" (or file not changed) */
   
  else if (yield > 0) main_filechanged = currentbuffer->changed = FALSE;
   
  return yield;
  }
}


/*************************************************
*           Check for all files not changed      *
*************************************************/

/* This is called when attempting to close down NE */

static BOOL allow_new_all(void)
{
int yield = TRUE;
bufferstr *oldbuffer = currentbuffer;
bufferstr *buffer = main_bufferchain;

while (buffer != NULL)
  {
  selectbuffer(buffer);
  if ((yield = allow_new()) == FALSE) break;
  buffer = buffer->next;
  }

if (oldbuffer != NULL) selectbuffer(oldbuffer);
return yield;
}



/*************************************************
*              Close a window                    *
*************************************************/

/* This can happen as a result of a mouse click, or after a command. If there
is only one buffer, don't bother throwing away the data, as the re-init will
throw away all the store anyway. When called from a command, buffer cannot
be a task window. When called from a mouse click, buffer is in fact always
equal to currentbuffer. */

int sys_window_close(bufferstr *buffer)
{
S_Key_Pressed sk;
int yield = done_continue;
int hadcaret;

/* If this is an active taskwindow, query. */

if (buffer->windowhandle2 != 0)
  {
  int rc;
  control_task(Message_TaskWindow_Suspend);

  /* If we've killed it before, give the option of forcing a window
  closure (but send another kill anyway). This just gives a way out
  of obscure messes. */

  if (buffer->killed && 
      sys_window_yesno(FALSE, "Force close", "Task still active after killing"))
    {
    control_task(Message_TaskWindow_Morite);   /* Just in case */
    goto CLOSE;
    }

  rc = sys_window_yesno(FALSE, "Kill Task", "Task still active");
  if (rc)
    {
    control_task(Message_TaskWindow_Morite);
    buffer->killed = TRUE;
    }
  else control_task(Message_TaskWindow_Resume);
  return done_error;
  }

CLOSE:

/* Carry on with the closing */

SWI(Wimp_GetCaretPosition, 2, 0, (int)(&sk));
hadcaret = sk.handle == buffer->windowhandle;

/* If this is the only buffer, no need to free things, as there will be a
global reinitialization. */

if (buffer == main_bufferchain && buffer->next == NULL)
  {
  SWI(Wimp_DeleteWindow, 2, 0, (int)(&(buffer->windowhandle)));
  handle_etext = -1;
  currentbuffer = main_bufferchain = NULL;
  }
else
  {
  int handle = buffer->windowhandle;
  char *title = buffer->windowtitle;
  if ((yield = setup_dbuffer(buffer)) == done_continue)
    {
    SWI(Wimp_DeleteWindow, 2, 0, (int)(&handle));
    store_free(title);
    handle_etext = currentbuffer->windowhandle;
    if (hadcaret)
      SWI(Wimp_SetCaretPosition, 6, handle_etext, -1, -100, -100, 12, -1);
    }
  }
  
return yield;
}


/*************************************************
*              Close all windows                 *
*************************************************/

/* Called by the STOP & W commands. By this stage, any checks on whether
to proceed because of unmodified buffers have been done. We also do not
need to waste time freeing the individual lines in the buffers, since
all the allocated store is going to be freed. */

static void window_close_all(void)
{
currentbuffer = main_bufferchain;
while (currentbuffer != NULL)
  {
  SWI(Wimp_DeleteWindow, 2, 0, (int)(&(currentbuffer->windowhandle)));
  currentbuffer = currentbuffer->next;
  }
main_bufferchain = NULL;
}



/*************************************************
*          Buffer has been reloaded              *
*************************************************/

/* Called by the LOAD command */

void sys_window_load(void)
{
sys_window_showmark(mark_unset, FALSE);
while (file_extend() != NULL);
old_linecount = -1;
set_extent();
open_window(handle_etext, FALSE, TRUE);
}



/*************************************************
*          Start off editing a file              *
*************************************************/

int sys_window_start_off(char *name)
{
int oldhandle = handle_etext;
BOOL failed = FALSE;

/* Set up the window for the new buffer, as various functions
that are about to be called will assume its existence. */

window_etext->title_data.ind.text = store_Xget(window_title_len);
window_etext->title_data.ind.text[0] = 0;
handle_etext = SWI(Wimp_CreateWindow, 2, 0, (int)window_etext);

/* If in the quiescent state, initialize things */

if (currentbuffer == NULL)
  {
  main_readonly = FALSE;            /* Always start off not read-only */
  if (init_init(NULL, name, default_filetype, name, FALSE))
    {
    if (!main_noinit && main_einit != NULL) cmd_obey(main_einit);
    main_initialized = TRUE;
    }
  else failed = TRUE;
  }

/* Otherwise treat as a "newbuffer" command; the new buffer
is automatically made current. */

else if (setup_newbuffer(name) != done_continue) failed = TRUE;

/* If something went wrong, kill the new window and re-instate
the old one as current. */

if (failed)
  {
  SWI(Wimp_DeleteWindow, 2, 0, (int)&handle_etext);
  store_free(window_etext->title_data.ind.text);
  handle_etext = oldhandle;
  return FALSE;
  }

/* Otherwise we have a new buffer selected. Remember the window
handle; read in the file, set up and open the window. */

currentbuffer->windowhandle = handle_etext;
currentbuffer->windowhandle2 = 0;
currentbuffer->lastwascr = 0;
currentbuffer->windowtitle = window_etext->title_data.ind.text;

sys_window_load();
sys_display_cursor(TRUE);
SWI(Wimp_SetCaretPosition, 6, handle_etext, -1, -100, -100, 12, -1);
update_position(TRUE); 
return TRUE;
}



/*************************************************
*        Put up an error window                  *
*************************************************/

void sys_werr(char *s)
{
display_error("%s", s);
}


/*************************************************
*              Set filetype                      *
*************************************************/

/* Set the save_buffer's file type from the field in the
save window. Return FALSE if bad. */

static int set_filetype(void)
{
char buff[20];
int val = 0;
char *endptr;
char *type = get_icon_text(handle_save, icon_savetype);

swi_regs.r[3] = 0;
while (ESWI(OS_ReadVarVal,5,(int)"File$Type_*",(int)buff,20,swi_regs.r[3],0) == NULL)
  {
  char *s = buff;
  char *t = type;  
  buff[swi_regs.r[2]] = 0; 
  while ((*t || *s) && tolower(*t) == tolower(*s)) { t++; s++; }
  if (*t == 0 && *s == 0)
    {
    type = (char *)(swi_regs.r[3]) + 10;
    break;
    }  
  } 

val = (int)strtoul(type, &endptr , 16);
if (*endptr)
  {
  display_error("Bad file type: %s", type);
  return FALSE;  
  }
else
  {
  save_buffer->filetype = val;
  return TRUE;
  }       
}



/*************************************************
*             Click in a window                  *
*************************************************/

static void window_click(S_Mouse_Click *sm)
{
if (sm->handle == handle_etext)
  {
  if (sm->buttons == 2)
    {
    char buff[20]; 
    char *typename; 
    icon_state_block isb;
    isb.handle = handle_save;
    isb.icon = icon_saveicon;
    SWI(Wimp_GetIconState, 2, 0, (int)(&isb));
     
    sprintf(isb.ib.id.ind.text+5, "%03x", currentbuffer->filetype);
    
    if (ESWI(Wimp_SpriteOp, 3, 40, 0, (int)(isb.ib.id.ind.text)) != NULL)
      strcpy(isb.ib.id.ind.text+5, "xxx");  
    
    sprintf(buff, "File$Type_%03X", currentbuffer->filetype);
    typename = getenv(buff);
    if (typename == NULL) typename = buff + 10; 
    set_icon_text(handle_save, icon_savetype, typename);  

    set_icon_text(handle_save, icon_savename,
      (main_filealias == NULL || main_filealias[0] == 0)? "TextFile" : main_filealias);
      
    if (currentbuffer->windowhandle2 == 0)
      {
      menu_textwindow.item_width = 100;
      menu_textwindow.items[textmenu_killtask - 1].flags |= F_last;
      }
    else
      {
      menu_textwindow.item_width = 150;
      menu_textwindow.items[textmenu_killtask - 1].flags &= ~F_last;
      }

    SWI(Wimp_CreateMenu, 4, 0, (int)(&menu_textwindow), sm->x - 68, sm->y);
    menu_handle = sm->handle;
    save_buffer = currentbuffer;
    }
  else
    {
    int wax, way;
    window_state_block ws;
    ws.handle = handle_etext;
    SWI(Wimp_GetWindowState, 2, 0, (int)(&ws));

    wax = (sm->x - (ws.minx - ws.scrollx))/char_width;
    way = 0 - (sm->y - (ws.maxy - ws.scrolly))/char_height;

    sys_display_cursor(FALSE);

    while (way < main_currentlinenumber)
      {
      if (main_current->prev == NULL) break;
      main_current = main_current->prev;
      main_currentlinenumber--;
      }

    while (way > main_currentlinenumber)
      {
      if (main_current->next == NULL) break;
      main_current = main_current->next;
      main_currentlinenumber++;
      }

    cursor_col = wax;
    sys_display_cursor(TRUE);
    SWI(Wimp_SetCaretPosition, 6, handle_etext, -1, -100, -100, 12, -1);
    sys_window_showmark(mark_type, mark_hold);
    }
  }

else if (sm->handle == handle_save)
  {
  if (sm->buttons == 2) return;
  if (sm->icon == icon_saveOK)
    {
    char *s = get_icon_text(handle_save, icon_savename);
    if (strstr(s, "$.") != NULL || strstr(s, "<") != NULL) save_file(s);
      else display_error("To save, drag the icon to a directory display");
    }

  /* A drag on the sprite starts a drag to a filer window */

  else if (sm->icon == icon_saveicon && sm->buttons == 64 && set_filetype())
    {
    window_state_block wsb;
    icon_state_block isb;
    drag_block db;

    isb.handle = handle_save;
    isb.icon = icon_saveicon;
    SWI(Wimp_GetIconState, 2, 0, (int)(&isb));
    wsb.handle = handle_save;
    SWI(Wimp_GetWindowState, 2, 0, (int)(&wsb));
    db.type = 5;
    db.minx = isb.ib.minx + wsb.minx;
    db.miny = isb.ib.miny + wsb.maxy;
    db.maxx = isb.ib.maxx + wsb.minx;
    db.maxy = isb.ib.maxy + wsb.maxy;
    db.minxp = 0;
    db.minyp = 0;
    db.maxxp = 999999;
    db.maxyp = 999999;
    if (use_dragasprite)
      {
      char name[12];
      strncpy(name, isb.ib.id.ind.text, 8);
      name[8] = 0;
      SWI(DragASprite_Start, 4, 0x85, 1, (int)name, &db.minx);
      }
    else SWI(Wimp_DragBox, 2, 0, (int)(&db));
    }
  }
}



/*************************************************
*            Iconbar menu handler                *
*************************************************/

static void iconbar_menu_handler(int *items)
{
S_Mouse_Click s;
SWI(Wimp_GetPointerInfo, 2, 0, (int)(&s));

switch(items[0])
  {
  case iconbar_position:
  flip_position(); 
  break;  

  case iconbar_taskwindow:
  SWI(Wimp_StartTask, 1, (int)"taskwindow \"echo\"");
  break;

  case iconbar_quit:
  if (allow_new_all()) application_done = TRUE;
  break;
  }
if ((s.buttons & 1) != 0) 
  SWI(Wimp_CreateMenu, 4, 0, (int)(&menu_iconbar), 0, 0);
}


/*************************************************
*           Window menu handler                  *
*************************************************/

/* The save entry is handled by a separate window */

static void window_menu_handler(int *items)
{
switch (items[0])
  {
  case textmenu_killtask:
  control_task(Message_TaskWindow_Morite);
  break;
  }
}




/*************************************************
*        Handle click on the icon bar            *
*************************************************/

static void iconbar_click(S_Mouse_Click *s)
{
if (s->buttons == 2)
  {
  SWI(Wimp_CreateMenu, 4, 0, (int)(&menu_iconbar), s->x - 68,
   menu_item_height*(iconbar_quit + 2) + 52);
 menu_handle = -1;
  }
else if (s->buttons == 1) 
  SWI(Wimp_StartTask, 1, (int)"taskwindow \"echo\"");
else sys_window_start_off(NULL);
}




/*************************************************
*            Select these routines               *
*************************************************/

static void wros_dummy(void)
{
}

static void wros_select(void)
{
s_cls = (void (*)(void))wros_dummy;
s_defwindow = (void (*)(int, int, int))wros_dummy;
s_eraseright = (void (*)(void))wros_dummy;
s_flush = (void (*)(void))wros_dummy;
s_hscroll = (void (*)(int, int, int, int, int))wros_dummy;
s_init = (void (*)(int, int, BOOL))wros_dummy;
s_maxx = (int (*)(void))wros_dummy;
s_maxy = (int (*)(void))wros_dummy;
s_move = (void (*)(int, int))wros_dummy;
s_overstrike = (void (*)(int))wros_dummy;
s_printf = (void (*)(char *, ...))wros_dummy;
s_putc = (void (*)(int))wros_dummy;
s_rendition = (void (*)(int))wros_dummy;
s_selwindow = (void (*)(int, int, int))wros_dummy;
s_settabs = (void (*)(int *))wros_dummy;
s_terminate = (void (*)(void))wros_dummy;
s_vscroll = (void (*)(int, int, int, BOOL))wros_dummy;
s_window = (int (*)(void))wros_dummy;
s_x = (int (*)(void))wros_dummy;
s_y = (int (*)(void))wros_dummy;
}


/*************************************************
*             Run E in a window                  *
*************************************************/

/* This procedure is called if E is entered with the -W flag,
which requests that it run as a RISC OS wimp application in
a window. */

void sys_runwindow(void)
{
BOOL passkey = FALSE;
int *sprite_area;
int pollreturn[64];
create_icon_block iconbar;

#ifdef SCRNDEBUG
scrndebug = fopen("screenlog", "w", NULL);
#endif

/* Refer to the menu items that follow the main menus, to stop
the compiler complaining about unused variables. */

menu_iconbar_rest[0] = menu_iconbar_rest[0];
menu_textwindow_rest[0] = menu_textwindow_rest[0];

/* Grey-out the task window starter if not wanted */

if (!support_taskwindows)
  menu_iconbar.items[iconbar_taskwindow].iflags |= F_shaded;

/* Initialise this as a wimp task. Use the RISC OS 3 facility of
ignoring messages we are not interested in. */

Application_Name = "NE";
SWI(Wimp_Initialise, 4, 310, 0x4B534154, (int)"NE",
  (int) wimp_message_list);

/* Set up block for creating the icon on the icon bar. Since this
is the only icon we create, we just do it by steam. */

iconbar.handle = -1;     /* => place on the right */
iconbar.ib.minx = 0;
iconbar.ib.maxx = 66;
iconbar.ib.miny = 0;
iconbar.ib.maxy = 72;
iconbar.ib.flags = 0x17003002;
strcpy(iconbar.ib.id.name, "!NE");
SWI(Wimp_CreateIcon, 2, 0, (int)(&iconbar));

/* Set the DragASprite option from the CMOS */

SWI(OS_Byte, 2, 161, 28);
use_dragasprite = (swi_regs.r[2] & 2) != 0;

/* Load in NE's private tiling sprite for Risc PC */

SWI(OS_File, 2, 5, (int)"<NE$Dir>.TileSprite");
sprite_area = malloc(swi_regs.r[4]+100);
sprite_area[0] = swi_regs.r[4] + 100;
sprite_area[2] = 16;
SWI(OS_SpriteOp, 2, 256+9, (int)sprite_area);
SWI(OS_SpriteOp, 3, 256+10, (int)sprite_area, (int)"<NE$Dir>.TileSprite");

/* Load and set up the template windows */

SWI(Wimp_OpenTemplate, 2, 0, (int)"<NE$Dir>.Templates");

if (load_template("Info", &window_info))
  {
  char buff[80];
  handle_info = SWI(Wimp_CreateWindow, 2, 0, (int)window_info);
  menu_iconbar.items[iconbar_info].submenu = (menu_block *)handle_info;
  sprintf(buff, "%s %s", version_string, version_date);
  set_icon_text(handle_info, icon_version, buff);
  }

if (load_template("cmdline", &window_cmdline))
  {
  handle_cmdline = SWI(Wimp_CreateWindow, 2, 0, (int)window_cmdline);
  }

if (load_template("query", &window_query))
  {
  window_query->sprite_area = sprite_area;
  handle_query = SWI(Wimp_CreateWindow, 2, 0, (int)window_query);
  }

if (load_template("closequery", &window_closequery))
  {
  window_closequery->sprite_area = sprite_area;
  handle_closequery = SWI(Wimp_CreateWindow, 2, 0, (int)window_closequery);
  }

if (load_template("position", &window_position))
  {
  window_position->sprite_area = sprite_area;
  handle_position = SWI(Wimp_CreateWindow, 2, 0, (int)window_position);
  }

if (load_template("save", &window_save))
  {
  window_save->sprite_area = sprite_area;
  handle_save = SWI(Wimp_CreateWindow, 2, 0, (int)window_save);
  menu_textwindow.items[textmenu_save].submenu = (menu_block *)handle_save;
  }

load_template("Etext", &window_etext);
load_template("Text", &window_text);

window_etext->sprite_area = sprite_area;
window_text->sprite_area = sprite_area;

SWI(Wimp_CloseTemplate, 0);

/* Connect the high-level display routines. */

wros_select();
main_screenOK = TRUE;

window_depth = 999999;
window_width = 999999;


/* If a file name is given (which happens when a file is double-
clicked), get it started, and load the whole file. */

if (arg_from_name != NULL) sys_window_start_off(arg_from_name);

main_initialized = TRUE;

/* Now enter the main polling loop; poll_mask is initialized as a
global variable, as is application_done. */

while(!application_done)
  {
  int key, pollaction;
  msg_data_block *ds = (msg_data_block *)pollreturn;
  msg_output_block *dso = (msg_output_block *)pollreturn;
  S_Mouse_Click *sm = (S_Mouse_Click *)pollreturn;
  S_Key_Pressed *sk = (S_Key_Pressed *)pollreturn;
  
  update_position(FALSE); 

  pollaction = SWI(Wimp_Poll, 4, poll_mask, (int)pollreturn, 0, 0);
  switch(pollaction)
    {
    case Close_Window_Request:
    if (pollreturn[0] == handle_text)
      {
      SWI(Wimp_CloseWindow, 2, 0, &handle_text);
      scrw_empty(handle_text, FALSE);
      text_written = FALSE;
      }
    else
      {
      bufferstr *buffer = findbuffer(pollreturn[0]);
      SWI(Wimp_CloseWindow, 2, 0, &handle_save);
      if (buffer != NULL && allow_new())
        {
        /* Check on unpasted cut buffer */ 
        if (currentbuffer->next == NULL && !cut_pasted && cut_buffer != 0 &&
          (cut_buffer->len != 0 || cut_buffer->next != NULL) && main_warnings)
          { 
          if (!sys_window_yesno(FALSE, "Continue",
            "The contents of the cut buffer have not been pasted.\n"))
              break;
          }      

        /* Close any associated stream output file */

        if (currentbuffer->to_fid != NULL)
          {
          fclose(currentbuffer->to_fid);
          currentbuffer->to_fid = NULL;
          }

        /* Close the window and delete the buffer; sys_window_close()
        selects a new buffer if there is one. */
         
        sys_window_close(buffer);

        /* Handle closure of the last window */
        if (currentbuffer == NULL)
          {
          SWI(Wimp_CloseWindow, 2, 0, &handle_position);
          if (handle_text >= 0)
            {
            SWI(Wimp_CloseWindow, 2, 0, &handle_text);
            scrw_empty(handle_text, FALSE);
            text_written = FALSE;
            }
          store_free_all();
          }
        }
      }
    break;


    /* Key presses can only come from the window with the focus;
    this must be the current window. The passkey flag is used to force
    NE to pass keys on to other applications. */

    case Key_Pressed:
    key = sk->key;

    if (passkey)
      {
      passkey = FALSE;
      SWI(Wimp_ProcessKey, 1, key);
      }

    /* Escape allows next key to be passed through */

    else if (key == 0x1B) passkey = TRUE;

    /* Sh/ct/F10 closes the error text window */

    else if (key == 0x1FA && handle_text >= 0)
      {
      SWI(Wimp_CloseWindow, 2, 0, &handle_text);
      scrw_empty(handle_text, FALSE);
      text_written = FALSE;
      }
      
    /* Sh/ct/F9 flips the line position info window */
    
    else if (key == 0x1B9)
      {
      flip_position(); 
      SWI(Wimp_CreateMenu, 4, 0, -1, 0, 0);
      }    

    /* Return in the save box triggers the save */

    else if (sk->handle == handle_save)
      {
      if (key == '\r' && set_filetype()) 
        {
        char *s = get_icon_text(handle_save, icon_savename);
        if (strstr(s, "$.") != NULL || strstr(s, "<") != NULL) save_file(s);
          else display_error("To save, drag the icon to a directory display");
        }   
      }

    /* Pass on keystrokes that we don't understand */

    else if (key >= 0xFF && ControlKeystroke[key-0x17F] == s_f_user)
      SWI(Wimp_ProcessKey, 1, key);

    /* Else must be known key in a text window. */

    else
      {
      error_count = 0;                     /* reset at each interaction */
      if (key == 8 && osbyte(121, 0xaf, 0) != 0) key = 0x203;
      if (key == 0x7F || key == 0x203)     /* fudges are required for backspace and delete */
        {
        int shift = osbyte(121, 0x80, 0)? 1 : 0;
        int ctrl = osbyte(121, 0x81, 0)? 2 : 0;
        if (key == 0x203) key += shift + ctrl; else           /* 0x203 - 0x206 */
          {
          if (shift + ctrl != 0) key = 0x1ff + shift + ctrl;  /* 0x200 - 0x202 */
            else key = 0x17F;                                 /* 0x17F */
          }
        }
      sys_display_cursor(FALSE);

      cursor_row = main_currentlinenumber;
      cursor_offset = 0;
      cursor_max = 999999;

      /* Handle keystroke in a normal window, or data and simple line 
      editing in a task window. */

      if (currentbuffer->windowhandle2 == 0 || 
          (key >= 32 && key <= 255) ||           /* Printing */
          (key == 0x17F && cursor_col > currentbuffer->outcursor) || /* Delete */ 
          (key >= 0x18A && key <= 0x18D) ||      /* Tab, Copy, left, right */
          (key >= 0x19A && key <= 0x19D) ||      /* ditto, shifted */
          (key >= 0x1AA && key <= 0x1AD))        /* ditto, ctrld */
        {
        osbyte(229, 0, 0);                       /* Enable escape */
        sys_keystroke(key);                      /* Handle the key */
        osbyte(229, 1, 0);                       /* Disable escape */
        osbyte(124, 0, 0);                       /* Clear escape condition */
        
        /* Don't let the cursor get into the prompt in a taskwindow */
         
        if (currentbuffer->windowhandle2 != 0 && cursor_col < currentbuffer->outcursor)
          cursor_col = currentbuffer->outcursor; 

        if (hourglass_on) sys_hourglass(FALSE);  /* can happen on error exit */
        }
        
      /* Up and down arrows retrieve previous commands */
      
      else if (key == 0x18E || key == 0x18F)
        {
        BOOL up = key == 0x18F; 
        linestr *ln = currentbuffer->lastarrow;
         
        if (ln == NULL) ln = up? main_current : main_top; 
         
        for (; ln != NULL; ln = up? ln->prev : ln->next)
          { 
          if ((ln->flags & lf_cmd) != 0)
            { 
            int clen = ln->len;
            char *p = ln->text;
          
            while (clen-- > 0)
              { if (*p++ == '*') break; }  
            if (clen <= 0 && p[-1] != '*') 
              { 
              clen = ln->len; 
              p = ln->text; 
              }
            if (clen <= 0 || 
                (clen == main_current->len - currentbuffer->outcursor && 
                 strncmp(p, main_current->text + currentbuffer->outcursor, clen) == 0))
              continue;    
             
            currentbuffer->lastarrow = ln;
            cursor_col = currentbuffer->outcursor;
            key_handle_function(s_f_delrt);
              
            while (clen-- > 0) key_handle_data(*p++);
            break; 
            } 
          }  
        }   
        
      /* Keystroke ^U in a taskwindow deletes the current command */
      
      else if (key == 21)
        {
        cursor_col = currentbuffer->outcursor;
        key_handle_function(s_f_delrt);
        }   

      /* Handle keystroke '\r' in a task window - everything else ignored */

      else if (key == '\r')
        {
        msg_data_block dss;                                   /* Done this way because the output */
        msg_output_block *ds = (msg_output_block *)(&dss);    /* block has char [99999] at the end */
         
        int clen = main_current->len - currentbuffer->outcursor;
        char *p = main_current->text + currentbuffer->outcursor;

        /* Mark the line as a command line, for later retrieval */
        
        main_current->flags |= lf_cmd; 
          
        /* Arrange not to reflect the command */
  
        currentbuffer->skipcount = clen;

        /* The manual has offset 24 (data) as a pointer to the data, 
        with the length in offset 20. It appears that in practice this 
        is wrong. (Volume 5a corrects the misprint.) The actual data is 
        in offset 24 - but only up to 3 chars? At least I can't make 
        more work. So just send them one by one. */  
          
        while (clen-- >= 0)
          {
          ds->length = 28;
          ds->action = Message_TaskWindow_Input;
          ds->data_size = 1; 
          ds->data[0] = (clen >= 0)? *p++ : '\r';
          SWI(Wimp_SendMessage, 3, User_Message, (int)(ds), currentbuffer->windowhandle2);
          }  
           
        /* Ensure cursor at line end, and reset the arrow line pointer. */

        cursor_col = main_current->len;
        currentbuffer->lastarrow = NULL; 
        }

      /* Tidy up the aftermath of the keystroke */

      if (main_done)                 /* Obeyed STOP or W */
        {
        SWI(Wimp_CloseWindow, 2, 0, &handle_position);
        window_close_all();
        main_done = FALSE;
        main_rc = 0;
        if (handle_text >= 0)
          {
          SWI(Wimp_CloseWindow, 2, 0, &handle_text);
          scrw_empty(handle_text, FALSE);
          text_written = FALSE;
          }
        }

      if (currentbuffer != NULL)    /* NULL => all text windows closed */
        {
        sys_window_display(TRUE, TRUE);
        sys_display_cursor(TRUE);
        if (currentbuffer->windowhandle2 != 0) main_filechanged = FALSE;
        }
      else 
        {
        SWI(Wimp_CloseWindow, 2, 0, &handle_position);
        store_free_all();
        } 
      }
    break;

    case Gain_Caret:
    case Lose_Caret:
      {
      bufferstr *oldbuffer = currentbuffer;
      bufferstr *buffer = findbuffer(pollreturn[0]);
      if (buffer != NULL)
        {
        sys_display_cursor(pollaction == Gain_Caret);
        if (buffer != oldbuffer) selectbuffer(oldbuffer);
        }
      }
    break;

    case Menu_Selection:
    if (menu_handle < 0) iconbar_menu_handler(pollreturn);
      else window_menu_handler(pollreturn);
    break;

    /* Mouse clicks can come from any window. If we can't find a
    related buffer, findbuffer() returns NULL, but that is OK. */

    case Mouse_Click:
    if (sm->handle < 0) iconbar_click(sm); else
      {
      bufferstr *oldbuffer = currentbuffer;
      bufferstr *buffer = findbuffer(sm->handle);
    
      /* For task window, just grab the focus. Otherwise process the
      click to position the cursor. */
        
      if (buffer->windowhandle2 != 0) 
        SWI(Wimp_SetCaretPosition, 6, handle_etext, -1, -100, -100, 12, -1);
      else window_click(sm);
 
      if (sm->buttons == 2 && buffer != NULL && buffer != oldbuffer)
        selectbuffer(oldbuffer);
      }
    break;

    case Open_Window_Request:
    SWI(Wimp_OpenWindow, 2, 0, (int)pollreturn);
    break;

    /* Redraw requests can come from any window. Ensure that it is the
    current window during the redraw. */

    case Redraw_Window_Request:
    if (pollreturn[0] == handle_text) scrw_poll(pollaction, pollreturn); else
      {
      bufferstr *oldbuffer = currentbuffer;
      bufferstr *buffer = findbuffer(pollreturn[0]);
      wros_redraw_window();
      if (buffer != oldbuffer) selectbuffer(oldbuffer);
      }
    break;

    /* This happens when saving from the text window by dragging
    the icon somewhere. */

    case User_Drag_Box:
      {
      S_Mouse_Click s;
      msg_data_block mb;
      window_state_block ws;

      char *savename = get_icon_text(handle_save, icon_savename);
      char *fn = savename + strlen(savename);

      if (use_dragasprite) SWI(DragASprite_Stop, 0);

      /* If the save window was killed by escape while dragging, the
      drag doesn't stop. We have to check for the window's closure
      here. */  
      
      ws.handle = handle_save;
      SWI(Wimp_GetWindowState, 2, 0, (int)(&ws));
      if ((ws.flags & 0x10000) == 0) break;

      /* Get pointer to leaf name */

      while (fn > savename && fn[-1] != '.') fn--;

      SWI(Wimp_GetPointerInfo, 2, 0, (int)(&s));

      if (s.handle != handle_save)
        {
        mb.length = sizeof(msg_data_block);
        mb.your_ref = 0;
        mb.action = Message_DataSave;
        mb.handle = s.handle;
        mb.icon = s.icon;
        mb.x = s.x;
        mb.y = s.y;
        mb.size = 0;
        mb.filetype = 0xfff;
        strcpy(mb.name, fn);

        if (s.handle == -2)
          SWI(Wimp_SendMessage, 4, User_Message, (int)(&mb), s.handle, s.icon);
            else SWI(Wimp_SendMessage, 3, User_Message, (int)(&mb), s.handle);
        }
      }
    break;

    case User_Message:
    case User_Message_Recorded:

    /*****
    debug_printf("Message: sender %x my_ref %x your_ref %x action %x\n",
      ds->sender, ds->my_ref, ds->your_ref, ds->action);
    debug_printf("handle %x icon %x x %x y %x size %x filetype %x\n",
      ds->handle, ds->icon, ds->x, ds->y, ds->size, ds->filetype);
    *****/

    switch(ds->action)
      {
      /* DataOpen and DataLoad can by handled by virtually the same
      code. The former occurs when any file is double clicked; the
      latter when a file is dragged to our iconbar icon, or to an
      existing open window. */

      case Message_DataOpen:
      if (ds->filetype != 0xfff) break;
      /* Otherwise, fall through */

      case Message_DataLoad:
        {
        bufferstr *oldbuffer = currentbuffer;
        bufferstr *this = findbuffer(ds->handle);
        ds->your_ref = ds->my_ref;
        ds->action = Message_DataLoadAck;
        SWI(Wimp_SendMessage, 3, User_Message, (int)ds, ds->sender);

        /* If not dragged to one of our windows, open a new window
        for the file; otherwise mock up an "insert" command. */

        if (this == NULL) sys_window_start_off(ds->name); else
          {
          char cmdline[100];
          sprintf(cmdline, "unless sol do sa//; i %s", ds->name);
          sys_display_cursor(FALSE);
          cmd_obey(cmdline);
          if (currentbuffer != oldbuffer) selectbuffer(oldbuffer);
          set_extent();
          sys_display_cursor(TRUE);
          }
        }
      break;

      /* DataSave occurs when a save icon has been dragged from another
      task to our iconbar icon. We tell the task to use Wimp$Scrap. Make
      sure the length field is correct. */

      case Message_DataSave:
      ds->length = sizeof(msg_data_block) + 4;
      ds->your_ref = ds->my_ref;
      ds->action = Message_DataSaveAck;
      ds->size = -1;
      strcpy(ds->name, "<Wimp$Scrap>");
      SWI(Wimp_SendMessage, 3, User_Message, (int)ds, ds->sender);
      break;

      /* DataSaveAck comes when the save icon has been dragged to a window and
      the name of the file to write is being returned. Write the file, then
      send DataLoad so that if it's another application, it will load it. */

      case Message_DataSaveAck:
        {
        save_file(ds->name);
        ds->action = Message_DataLoad;
        ds->your_ref = ds->my_ref;
        SWI(Wimp_SendMessage, 3, User_Message, (int)ds, ds->sender);
        }
      break;

      /* Note: if there is any chance of not closing down, you have to acknowledge
      the message first, to stop the Wimp going ahead, even before you ask the
      user. If the user subsequently allows the close down, you then have to
      re-start the sequence. This makes life messy... */

      case Message_PreQuit:
      if (currentbuffer != NULL)
        {
        ds->your_ref = ds->my_ref;
        SWI(Wimp_SendMessage, 3, User_Message_Acknowledge, (int)ds, ds->sender);

        if (allow_new_all())
          {
          int send_to = ds->sender;
          SWI(Wimp_GetCaretPosition, 2, 0, (int)sk);
          sk->key = 0x1FC;
          SWI(Wimp_SendMessage, 3, Key_Pressed, (int)sk, send_to);

          window_close_all();
          if (handle_text >= 0)
            {
            SWI(Wimp_CloseWindow, 2, 0, &handle_text);
            scrw_empty(handle_text, FALSE);
            text_written = FALSE;
            }
          store_free_all();
          }
        }
      break;

      case Message_Quit:
      application_done = TRUE;
      break;

      /* Handle taskwindows. Note that this protocol is not as documented
      in the PRM. It is an arcane bit of jiggery-pokery which I made work
      by experiment. */

      case Message_TaskWindow_NewTask:
      if (support_taskwindows)
        {
        ds->your_ref = ds->my_ref;
        SWI(Wimp_SendMessage, 3, User_Message_Acknowledge, (int)ds, ds->sender);

        /* We first open a new window, and set its "windowhandle2" field to its
        own address, which will be unique. Then use that to identify it later
        from the Ego message. */

        sys_window_start_off(NULL);
        currentbuffer->windowhandle2 = (int)currentbuffer;
        currentbuffer->killed = FALSE;
        currentbuffer->skipcount = 0; 
        currentbuffer->outcursor = 0; 
        currentbuffer->lastarrow = NULL; 
        main_rmargin += MAX_RMARGIN;     /* Turn off margin */

        if (main_filealias != main_filename) store_free(main_filealias);
        currentbuffer->filealias = main_filealias = store_copystring("<Task Window>");

        /* Add to the supplied command the id of this task and a text handle
        for identifying the window. This must be *precisely* in this format.
        Then "start" the task (actually, it's already started...) */
          {
          char *s = (char *)(&ds->handle);
          sprintf(s + strlen(s), " %0.8X %0.8X ",
            SWI(Wimp_ReadSysInfo, 1, 5), (int)currentbuffer);
          SWI(Wimp_StartTask, 1, (int)s);
          }
        }
      break;


      /* A new task tells us its task_id. Replace the text id in the
      buffer block with the task id. */

      case Message_TaskWindow_Ego:
        {
        bufferstr *oldbuffer = currentbuffer;
        bufferstr *this = findtaskbuffer(ds->handle);
        if (this != NULL)
          {
          currentbuffer->windowhandle2 = ds->sender;
          if (currentbuffer != oldbuffer) selectbuffer(oldbuffer);
          }
        }
      break;


      /* A task is dying. Put a message in its window, and reset
      the windowhandle2 to zero, to permit window closure. However,
      if this was the result of an attempted close, just get on
      with it. */

      case Message_TaskWindow_Morio:
        {
        bufferstr *this = findtaskbuffer(dso->sender);
        if (this->killed)
          {
          this->windowhandle2 = 0;
          sys_window_close(this);
          if (currentbuffer == NULL) store_free_all();
          break;  
          }
        }     
 
      /* Fall through to write some text */

      strcpy(dso->data, "\n----- End -----  ");
      dso->data_size = strlen(dso->data);

      /* Handle taskwindow output. Ignore one carriage return after a
      line feed, and vice versa. Otherwise treat either one as starting
      a new line. Sadly, all characters written by the program don't get
      passed though to here. In particular, bare carriage returns do not
      make it - though they come from reflected input. Therefore, apply
      a forced line wrap at the window width minus a bit. 
      
      If skipcount is set we are expecting reflection of input, which 
      should not be re-reflected. */

      case Message_TaskWindow_Output:
        {
        bufferstr *oldbuffer = currentbuffer;
        bufferstr *this = findtaskbuffer(dso->sender);
        if (this != NULL)
          {
          int i;
          BOOL old_filechanged = main_filechanged;
          BOOL old_AutoAlign = main_AutoAlign;
          char *s = dso->data;

          if (ds->action == Message_TaskWindow_Morio)
            {
            if (currentbuffer->col == 0) { s++; dso->data_size--; }
            currentbuffer->windowhandle2 = 0;
            }

          sys_display_cursor(FALSE);
          cursor_offset = 0;
          cursor_max = 999999;
          tw_change = TRUE;
          main_AutoAlign = FALSE;
          
          /* Now process the characters */

          for (i = 0; i < dso->data_size; i++)
            {
            int ch = *s++;
            
            if (currentbuffer->skipcount-- > 0) continue;
 
            if (ch == '\n')
              {
              if (currentbuffer->lastwascr == '\r')
                { currentbuffer->lastwascr = 0; continue; }
              cursor_row = main_currentlinenumber;
              key_handle_function('\r');
              currentbuffer->lastwascr = '\n';
              currentbuffer->outcursor = 0; 
              }

            else if (ch == '\r')
              {
              if (currentbuffer->lastwascr == '\n')
                { currentbuffer->lastwascr = 0; continue; }
              cursor_row = main_currentlinenumber;
              key_handle_function(ch);
              currentbuffer->lastwascr = '\r';
              currentbuffer->outcursor = 0; 
              }

            else if (ch == 0x7f)
              {
              currentbuffer->lastwascr = 0;
              cursor_row = main_currentlinenumber;
              key_handle_function(s_f_del);
              }

            else
              {
              char *p = s - 1;
              currentbuffer->lastwascr = 0;
              if (cursor_col > wimp_window_width - 50) key_handle_function('\r');
              while (i < dso->data_size - 1 && *s != '\n' && *s != '\r')
                { i++; s++; }
              line_insertch(main_current, cursor_col, p, s - p, 0);
              sys_window_display_line(main_currentlinenumber, cursor_col);
              cursor_col += s - p;
              currentbuffer->outcursor = cursor_col; 
              }
            }

          main_filechanged = old_filechanged;
          main_AutoAlign = old_AutoAlign;
          sys_window_display(TRUE, FALSE);
          if (currentbuffer != oldbuffer) selectbuffer(oldbuffer);
            else sys_display_cursor(TRUE);
          tw_change = FALSE;
          }
        }
      break;
      }
    break;
    }

  /* Ensure the text window is at the front if any messages were generated
  while processing the event. */

  if (text_written) { open_window(handle_text, TRUE, FALSE); text_written = FALSE; }
  }

/* Before leaving the Wimp, ensure that all the mouse buttons are up. This
papers over the RISC OS 3 bug which causes the machine to crash if exiting
happens as the result of pressing a button which has to be re-drawn when
the mouse button is released. */

for (;;)
  {
  S_Mouse_Click s;
  SWI(Wimp_GetPointerInfo, 2, 0, (int)(&s));
  if (s.buttons == 0) break;
  SWI(Wimp_Poll, 4, M_All - M_Null_Reason_Code, (int)pollreturn, 0, 0);
  }

/* Now we can gracefully exit */

SWI(Wimp_CloseDown, 2, 0, 0);
}

/* End of wros.c */

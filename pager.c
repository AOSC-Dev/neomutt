/**
 * @file
 * GUI display a file/email/help in a viewport with paging
 *
 * @authors
 * Copyright (C) 1996-2002,2007,2010,2012-2013 Michael R. Elkins <me@mutt.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page pager GUI display a file/email/help in a viewport with paging
 *
 * GUI display a file/email/help in a viewport with paging
 */

#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <regex.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include "mutt/mutt.h"
#include "config/lib.h"
#include "email/lib.h"
#include "mutt.h"
#include "pager.h"
#include "alias.h"
#include "color.h"
#include "commands.h"
#include "context.h"
#include "curs_lib.h"
#include "format_flags.h"
#include "globals.h"
#include "hdrline.h"
#include "hook.h"
#include "index.h"
#include "keymap.h"
#include "mailbox.h"
#include "mutt_attach.h"
#include "mutt_curses.h"
#include "mutt_header.h"
#include "mutt_logging.h"
#include "mutt_menu.h"
#include "mutt_window.h"
#include "muttlib.h"
#include "mx.h"
#include "ncrypt/ncrypt.h"
#include "opcodes.h"
#include "options.h"
#include "protos.h"
#include "recvattach.h"
#include "recvcmd.h"
#include "send.h"
#include "status.h"
#include "terminal.h"
#ifdef USE_SIDEBAR
#include "sidebar.h"
#endif
#ifdef USE_NNTP
#include "nntp/nntp.h"
#endif
#ifdef ENABLE_NLS
#include <libintl.h>
#endif

/* These Config Variables are only used in pager.c */
bool C_AllowAnsi; ///< Config: Allow ANSI colour codes in rich text messages
bool C_HeaderColorPartial; ///< Config: Only colour the part of the header matching the regex
short C_PagerContext; ///< Config: Number of lines of overlap when changing pages in the pager
short C_PagerIndexLines; ///< Config: Number of index lines to display above the pager
bool C_PagerStop; ///< Config: Don't automatically open the next message when at the end of a message
short C_SearchContext; ///< Config: Context to display around search matches
short C_SkipQuotedOffset; ///< Config: Lines of context to show when skipping quoted text
bool C_SmartWrap; ///< Config: Wrap text at word boundaries
struct Regex *C_Smileys; ///< Config: Regex to match smileys to prevent mistakes when quoting text
bool C_Tilde; ///< Config: Character to pad blank lines in the pager

#define ISHEADER(x) ((x) == MT_COLOR_HEADER || (x) == MT_COLOR_HDEFAULT)

#define IsAttach(pager) (pager && (pager)->body)
#define IsMsgAttach(pager)                                                     \
  (pager && (pager)->fp && (pager)->body && (pager)->body->email)
#define IsEmail(pager) (pager && (pager)->email && !(pager)->body)

static const char *Not_available_in_this_menu =
    N_("Not available in this menu");
static const char *Mailbox_is_read_only = N_("Mailbox is read-only");
static const char *Function_not_permitted_in_attach_message_mode =
    N_("Function not permitted in attach-message mode");

/* hack to return to position when returning from index to same message */
static int TopLine = 0;
static struct Email *OldHdr = NULL;

#define CHECK_MODE(test)                                                       \
  if (!(test))                                                                 \
  {                                                                            \
    mutt_flushinp();                                                           \
    mutt_error(_(Not_available_in_this_menu));                                 \
    break;                                                                     \
  }

#define CHECK_READONLY                                                         \
  if (!Context || Context->mailbox->readonly)                                  \
  {                                                                            \
    mutt_flushinp();                                                           \
    mutt_error(_(Mailbox_is_read_only));                                       \
    break;                                                                     \
  }

#define CHECK_ATTACH                                                           \
  if (OptAttachMsg)                                                            \
  {                                                                            \
    mutt_flushinp();                                                           \
    mutt_error(_(Function_not_permitted_in_attach_message_mode));              \
    break;                                                                     \
  }

#define CHECK_ACL(aclbit, action)                                              \
  if (!Context || !(Context->mailbox->rights & aclbit))                        \
  {                                                                            \
    mutt_flushinp();                                                           \
    /* L10N: %s is one of the CHECK_ACL entries below. */                      \
    mutt_error(_("%s: Operation not permitted by ACL"), action);               \
    break;                                                                     \
  }

/**
 * struct QClass - Style of quoted text
 */
struct QClass
{
  size_t length;
  int index;
  int color;
  char *prefix;
  struct QClass *next, *prev;
  struct QClass *down, *up;
};

/**
 * struct Syntax - Highlighting for a line of text
 */
struct Syntax
{
  int color;
  int first;
  int last;
};

/**
 * struct Line - A line of text in the pager
 */
struct Line
{
  LOFF_T offset;
  short type;
  short continuation;
  short chunks;
  short search_cnt;
  struct Syntax *syntax;
  struct Syntax *search;
  struct QClass *quote;
  unsigned int is_cont_hdr; /**< this line is a continuation of the previous header line */
};

// clang-format off
typedef uint8_t AnsiFlags;      ///< Flags, e.g. #ANSI_OFF
#define ANSI_NO_FLAGS        0  ///< No flags are set
#define ANSI_OFF       (1 << 0) ///< Turn off colours and attributes
#define ANSI_BLINK     (1 << 1) ///< Blinking text
#define ANSI_BOLD      (1 << 2) ///< Bold text
#define ANSI_UNDERLINE (1 << 3) ///< Underlined text
#define ANSI_REVERSE   (1 << 4) ///< Reverse video
#define ANSI_COLOR     (1 << 5) ///< Use colours
// clang-format on

/**
 * struct AnsiAttr - An ANSI escape sequence
 */
struct AnsiAttr
{
  AnsiFlags attr; ///< Attributes, e.g. underline, bold, etc
  int fg;         ///< Foreground colour
  int bg;         ///< Background colour
  int pair;       ///< Curses colour pair
};

static short InHelp = 0;

/**
 * struct Resize - Keep track of screen resizing
 */
static struct Resize
{
  int line;
  bool search_compiled;
  bool search_back;
} *Resize = NULL;

#define NUM_SIG_LINES 4

/**
 * check_sig - Check for an email signature
 * @param s    Text to examine
 * @param info Line info array to update
 * @param n    First line to check
 * @retval  0 Success
 * @retval -1 Error
 */
static int check_sig(const char *s, struct Line *info, int n)
{
  int count = 0;

  while (n > 0 && count <= NUM_SIG_LINES)
  {
    if (info[n].type != MT_COLOR_SIGNATURE)
      break;
    count++;
    n--;
  }

  if (count == 0)
    return -1;

  if (count > NUM_SIG_LINES)
  {
    /* check for a blank line */
    while (*s)
    {
      if (!ISSPACE(*s))
        return 0;
      s++;
    }

    return -1;
  }

  return 0;
}

/**
 * comp_syntax_t - Search for a Syntax using bsearch
 * @param m1 Search key
 * @param m2 Array member
 * @retval -1 m1 precedes m2
 * @retval  0 m1 matches m2
 * @retval  1 m2 precedes m1
 */
static int comp_syntax_t(const void *m1, const void *m2)
{
  const int *cnt = (const int *) m1;
  const struct Syntax *stx = (const struct Syntax *) m2;

  if (*cnt < stx->first)
    return -1;
  if (*cnt >= stx->last)
    return 1;
  return 0;
}

/**
 * resolve_color - Set the colour for a line of text
 * @param line_info Line info array
 * @param n         Line Number (index into line_info)
 * @param cnt       If true, this is a continuation line
 * @param flags     Flags, see #PagerFlags
 * @param special   Flags, e.g. A_BOLD
 * @param a         ANSI attributes
 */
static void resolve_color(struct Line *line_info, int n, int cnt,
                          PagerFlags flags, int special, struct AnsiAttr *a)
{
  int def_color;         /* color without syntax highlight */
  int color;             /* final color */
  static int last_color; /* last color set */
  bool search = false;
  int m;
  struct Syntax *matching_chunk = NULL;

  if (!cnt)
    last_color = -1; /* force attrset() */

  if (line_info[n].continuation)
  {
    if (!cnt && C_Markers)
    {
      SETCOLOR(MT_COLOR_MARKERS);
      addch('+');
      last_color = ColorDefs[MT_COLOR_MARKERS];
    }
    m = (line_info[n].syntax)[0].first;
    cnt += (line_info[n].syntax)[0].last;
  }
  else
    m = n;
  if (flags & MUTT_PAGER_LOGS)
  {
    def_color = ColorDefs[(line_info[n].syntax)[0].color];
  }
  else if (!(flags & MUTT_SHOWCOLOR))
    def_color = ColorDefs[MT_COLOR_NORMAL];
  else if (line_info[m].type == MT_COLOR_HEADER)
    def_color = (line_info[m].syntax)[0].color;
  else
    def_color = ColorDefs[line_info[m].type];

  if ((flags & MUTT_SHOWCOLOR) && (line_info[m].type == MT_COLOR_QUOTED))
  {
    struct QClass *class = line_info[m].quote;

    if (class)
    {
      def_color = class->color;

      while (class && class->length > cnt)
      {
        def_color = class->color;
        class = class->up;
      }
    }
  }

  color = def_color;
  if ((flags & MUTT_SHOWCOLOR) && line_info[m].chunks)
  {
    matching_chunk = bsearch(&cnt, line_info[m].syntax, line_info[m].chunks,
                             sizeof(struct Syntax), comp_syntax_t);
    if (matching_chunk && (cnt >= matching_chunk->first) &&
        (cnt < matching_chunk->last))
    {
      color = matching_chunk->color;
    }
  }

  if ((flags & MUTT_SEARCH) && line_info[m].search_cnt)
  {
    matching_chunk = bsearch(&cnt, line_info[m].search, line_info[m].search_cnt,
                             sizeof(struct Syntax), comp_syntax_t);
    if (matching_chunk && (cnt >= matching_chunk->first) &&
        (cnt < matching_chunk->last))
    {
      color = ColorDefs[MT_COLOR_SEARCH];
      search = 1;
    }
  }

  /* handle "special" bold & underlined characters */
  if (special || a->attr)
  {
#ifdef HAVE_COLOR
    if ((a->attr & ANSI_COLOR))
    {
      if (a->pair == -1)
        a->pair = mutt_alloc_color(a->fg, a->bg);
      color = a->pair;
      if (a->attr & ANSI_BOLD)
        color |= A_BOLD;
    }
    else
#endif
        if ((special & A_BOLD) || (a->attr & ANSI_BOLD))
    {
      if (ColorDefs[MT_COLOR_BOLD] && !search)
        color = ColorDefs[MT_COLOR_BOLD];
      else
        color ^= A_BOLD;
    }
    if ((special & A_UNDERLINE) || (a->attr & ANSI_UNDERLINE))
    {
      if (ColorDefs[MT_COLOR_UNDERLINE] && !search)
        color = ColorDefs[MT_COLOR_UNDERLINE];
      else
        color ^= A_UNDERLINE;
    }
    else if (a->attr & ANSI_REVERSE)
    {
      color ^= A_REVERSE;
    }
    else if (a->attr & ANSI_BLINK)
    {
      color ^= A_BLINK;
    }
    else if (a->attr == ANSI_OFF)
    {
      a->attr = 0;
    }
  }

  if (color != last_color)
  {
    ATTRSET(color);
    last_color = color;
  }
}

/**
 * append_line - Add a new Line to the array
 * @param line_info Array of Line info
 * @param n         Line number to add
 * @param cnt       true, if line is a continuation
 */
static void append_line(struct Line *line_info, int n, int cnt)
{
  int m;

  line_info[n + 1].type = line_info[n].type;
  (line_info[n + 1].syntax)[0].color = (line_info[n].syntax)[0].color;
  line_info[n + 1].continuation = 1;

  /* find the real start of the line */
  for (m = n; m >= 0; m--)
    if (line_info[m].continuation == 0)
      break;

  (line_info[n + 1].syntax)[0].first = m;
  (line_info[n + 1].syntax)[0].last =
      (line_info[n].continuation) ? cnt + (line_info[n].syntax)[0].last : cnt;
}

/**
 * new_class_color - Create a new quoting colour
 * @param[in]     class   Class of quoted text
 * @param[in,out] q_level Quote level
 */
static void new_class_color(struct QClass *class, int *q_level)
{
  class->index = (*q_level)++;
  class->color = ColorQuote[class->index % ColorQuoteUsed];
}

/**
 * shift_class_colors - Insert a new quote colour class into a list
 * @param[in]     quote_list List of quote colours
 * @param[in]     new_class  New quote colour to inset
 * @param[in]     index      Index to insert at
 * @param[in,out] q_level    Quote level
 */
static void shift_class_colors(struct QClass *quote_list,
                               struct QClass *new_class, int index, int *q_level)
{
  struct QClass *q_list = quote_list;
  new_class->index = -1;

  while (q_list)
  {
    if (q_list->index >= index)
    {
      q_list->index++;
      q_list->color = ColorQuote[q_list->index % ColorQuoteUsed];
    }
    if (q_list->down)
      q_list = q_list->down;
    else if (q_list->next)
      q_list = q_list->next;
    else
    {
      while (!q_list->next)
      {
        q_list = q_list->up;
        if (!q_list)
          break;
      }
      if (q_list)
        q_list = q_list->next;
    }
  }

  new_class->index = index;
  new_class->color = ColorQuote[index % ColorQuoteUsed];
  (*q_level)++;
}

/**
 * cleanup_quote - Free a quote list
 * @param[out] quote_list Quote list to free
 */
static void cleanup_quote(struct QClass **quote_list)
{
  struct QClass *ptr = NULL;

  while (*quote_list)
  {
    if ((*quote_list)->down)
      cleanup_quote(&((*quote_list)->down));
    ptr = (*quote_list)->next;
    if ((*quote_list)->prefix)
      FREE(&(*quote_list)->prefix);
    FREE(quote_list);
    *quote_list = ptr;
  }
}

/**
 * classify_quote - Find a style for a string
 * @param[out] quote_list   List of quote colours
 * @param[in]  qptr         String to classify
 * @param[in]  length       Length of string
 * @param[out] force_redraw Set to true if a screen redraw is needed
 * @param[out] q_level      Quoting level
 * @retval ptr Quoting style
 */
static struct QClass *classify_quote(struct QClass **quote_list, const char *qptr,
                                     size_t length, bool *force_redraw, int *q_level)
{
  struct QClass *q_list = *quote_list;
  struct QClass *class = NULL, *tmp = NULL, *ptr = NULL, *save = NULL;
  char *tail_qptr = NULL;
  int offset, tail_lng;
  int index = -1;

  if (ColorQuoteUsed <= 1)
  {
    /* not much point in classifying quotes... */

    if (!*quote_list)
    {
      class = mutt_mem_calloc(1, sizeof(struct QClass));
      class->color = ColorQuote[0];
      *quote_list = class;
    }
    return *quote_list;
  }

  /* classify quoting prefix */
  while (q_list)
  {
    if (length <= q_list->length)
    {
      /* case 1: check the top level nodes */

      if (mutt_str_strncmp(qptr, q_list->prefix, length) == 0)
      {
        if (length == q_list->length)
          return q_list; /* same prefix: return the current class */

        /* found shorter prefix */
        if (!tmp)
        {
          /* add a node above q_list */
          tmp = mutt_mem_calloc(1, sizeof(struct QClass));
          tmp->prefix = mutt_mem_calloc(1, length + 1);
          strncpy(tmp->prefix, qptr, length);
          tmp->length = length;

          /* replace q_list by tmp in the top level list */
          if (q_list->next)
          {
            tmp->next = q_list->next;
            q_list->next->prev = tmp;
          }
          if (q_list->prev)
          {
            tmp->prev = q_list->prev;
            q_list->prev->next = tmp;
          }

          /* make q_list a child of tmp */
          tmp->down = q_list;
          q_list->up = tmp;

          /* q_list has no siblings for now */
          q_list->next = NULL;
          q_list->prev = NULL;

          /* update the root if necessary */
          if (q_list == *quote_list)
            *quote_list = tmp;

          index = q_list->index;

          /* tmp should be the return class too */
          class = tmp;

          /* next class to test; if tmp is a shorter prefix for another
           * node, that node can only be in the top level list, so don't
           * go down after this point */
          q_list = tmp->next;
        }
        else
        {
          /* found another branch for which tmp is a shorter prefix */

          /* save the next sibling for later */
          save = q_list->next;

          /* unlink q_list from the top level list */
          if (q_list->next)
            q_list->next->prev = q_list->prev;
          if (q_list->prev)
            q_list->prev->next = q_list->next;

          /* at this point, we have a tmp->down; link q_list to it */
          ptr = tmp->down;
          /* sibling order is important here, q_list should be linked last */
          while (ptr->next)
            ptr = ptr->next;
          ptr->next = q_list;
          q_list->next = NULL;
          q_list->prev = ptr;
          q_list->up = tmp;

          index = q_list->index;

          /* next class to test; as above, we shouldn't go down */
          q_list = save;
        }

        /* we found a shorter prefix, so certain quotes have changed classes */
        *force_redraw = true;
        continue;
      }
      else
      {
        /* shorter, but not a substring of the current class: try next */
        q_list = q_list->next;
        continue;
      }
    }
    else
    {
      /* case 2: try subclassing the current top level node */

      /* tmp != NULL means we already found a shorter prefix at case 1 */
      if (!tmp && (mutt_str_strncmp(qptr, q_list->prefix, q_list->length) == 0))
      {
        /* ok, it's a subclass somewhere on this branch */

        ptr = q_list;
        offset = q_list->length;

        q_list = q_list->down;
        tail_lng = length - offset;
        tail_qptr = (char *) qptr + offset;

        while (q_list)
        {
          if (length <= q_list->length)
          {
            if (mutt_str_strncmp(tail_qptr, (q_list->prefix) + offset, tail_lng) == 0)
            {
              /* same prefix: return the current class */
              if (length == q_list->length)
                return q_list;

              /* found shorter common prefix */
              if (!tmp)
              {
                /* add a node above q_list */
                tmp = mutt_mem_calloc(1, sizeof(struct QClass));
                tmp->prefix = mutt_mem_calloc(1, length + 1);
                strncpy(tmp->prefix, qptr, length);
                tmp->length = length;

                /* replace q_list by tmp */
                if (q_list->next)
                {
                  tmp->next = q_list->next;
                  q_list->next->prev = tmp;
                }
                if (q_list->prev)
                {
                  tmp->prev = q_list->prev;
                  q_list->prev->next = tmp;
                }

                /* make q_list a child of tmp */
                tmp->down = q_list;
                tmp->up = q_list->up;
                q_list->up = tmp;
                if (tmp->up->down == q_list)
                  tmp->up->down = tmp;

                /* q_list has no siblings */
                q_list->next = NULL;
                q_list->prev = NULL;

                index = q_list->index;

                /* tmp should be the return class too */
                class = tmp;

                /* next class to test */
                q_list = tmp->next;
              }
              else
              {
                /* found another branch for which tmp is a shorter prefix */

                /* save the next sibling for later */
                save = q_list->next;

                /* unlink q_list from the top level list */
                if (q_list->next)
                  q_list->next->prev = q_list->prev;
                if (q_list->prev)
                  q_list->prev->next = q_list->next;

                /* at this point, we have a tmp->down; link q_list to it */
                ptr = tmp->down;
                while (ptr->next)
                  ptr = ptr->next;
                ptr->next = q_list;
                q_list->next = NULL;
                q_list->prev = ptr;
                q_list->up = tmp;

                index = q_list->index;

                /* next class to test */
                q_list = save;
              }

              /* we found a shorter prefix, so we need a redraw */
              *force_redraw = true;
              continue;
            }
            else
            {
              q_list = q_list->next;
              continue;
            }
          }
          else
          {
            /* longer than the current prefix: try subclassing it */
            if (!tmp && (mutt_str_strncmp(tail_qptr, (q_list->prefix) + offset,
                                          q_list->length - offset) == 0))
            {
              /* still a subclass: go down one level */
              ptr = q_list;
              offset = q_list->length;

              q_list = q_list->down;
              tail_lng = length - offset;
              tail_qptr = (char *) qptr + offset;

              continue;
            }
            else
            {
              /* nope, try the next prefix */
              q_list = q_list->next;
              continue;
            }
          }
        }

        /* still not found so far: add it as a sibling to the current node */
        if (!class)
        {
          tmp = mutt_mem_calloc(1, sizeof(struct QClass));
          tmp->prefix = mutt_mem_calloc(1, length + 1);
          strncpy(tmp->prefix, qptr, length);
          tmp->length = length;

          if (ptr->down)
          {
            tmp->next = ptr->down;
            ptr->down->prev = tmp;
          }
          ptr->down = tmp;
          tmp->up = ptr;

          new_class_color(tmp, q_level);

          return tmp;
        }
        else
        {
          if (index != -1)
            shift_class_colors(*quote_list, tmp, index, q_level);

          return class;
        }
      }
      else
      {
        /* nope, try the next prefix */
        q_list = q_list->next;
        continue;
      }
    }
  }

  if (!class)
  {
    /* not found so far: add it as a top level class */
    class = mutt_mem_calloc(1, sizeof(struct QClass));
    class->prefix = mutt_mem_calloc(1, length + 1);
    strncpy(class->prefix, qptr, length);
    class->length = length;
    new_class_color(class, q_level);

    if (*quote_list)
    {
      class->next = *quote_list;
      (*quote_list)->prev = class;
    }
    *quote_list = class;
  }

  if (index != -1)
    shift_class_colors(*quote_list, tmp, index, q_level);

  return class;
}

static int braille_line = -1;
static int braille_col = -1;

/**
 * check_marker - Check that the unique marker is present
 * @param q Marker string
 * @param p String to check
 * @retval num Offset of marker
 */
static int check_marker(const char *q, const char *p)
{
  for (; *p == *q && *q && *p && *q != '\a' && *p != '\a'; p++, q++)
    ;
  return (int) (*p - *q);
}

/**
 * check_attachment_marker - Check that the unique marker is present
 * @param p String to check
 * @retval num Offset of marker
 */
static int check_attachment_marker(const char *p)
{
  return check_marker(AttachmentMarker, p);
}

/**
 * check_protected_header_marker - Check that the unique marker is present
 * @param p String to check
 * @retval num Offset of marker
 */
static int check_protected_header_marker(const char *p)
{
  return check_marker(ProtectedHeaderMarker, p);
}

/**
 * mutt_is_quote_line - Is a line of message text a quote?
 * @param[in]  line   Line to test
 * @param[out] pmatch Regex sub-matches
 * @retval true Line is quoted
 *
 * Checks if line matches the #C_QuoteRegex and doesn't match #C_Smileys.
 * This is used by the pager for calling classify_quote.
 */
int mutt_is_quote_line(char *line, regmatch_t *pmatch)
{
  bool is_quote = false;
  regmatch_t pmatch_internal[1], smatch[1];

  if (!pmatch)
    pmatch = pmatch_internal;

  if (C_QuoteRegex && C_QuoteRegex->regex &&
      (regexec(C_QuoteRegex->regex, line, 1, pmatch, 0) == 0))
  {
    if (C_Smileys && C_Smileys->regex &&
        (regexec(C_Smileys->regex, line, 1, smatch, 0) == 0))
    {
      if (smatch[0].rm_so > 0)
      {
        char c = line[smatch[0].rm_so];
        line[smatch[0].rm_so] = 0;

        if (regexec(C_QuoteRegex->regex, line, 1, pmatch, 0) == 0)
          is_quote = true;

        line[smatch[0].rm_so] = c;
      }
    }
    else
      is_quote = true;
  }

  return is_quote;
}

/**
 * resolve_types - Determine the style for a line of text
 * @param[in]  buf          Formatted text
 * @param[in]  raw          Raw text
 * @param[in]  line_info    Line info array
 * @param[in]  n            Line number (index into line_info)
 * @param[in]  last         Last line
 * @param[out] quote_list   List of quote colours
 * @param[out] q_level      Quote level
 * @param[out] force_redraw Set to true if a screen redraw is needed
 * @param[in]  q_classify   If true, style the text
 */
static void resolve_types(char *buf, char *raw, struct Line *line_info, int n,
                          int last, struct QClass **quote_list, int *q_level,
                          bool *force_redraw, bool q_classify)
{
  struct ColorLine *color_line = NULL;
  struct ColorLineHead *head = NULL;
  regmatch_t pmatch[1];
  bool found;
  bool null_rx;
  int offset, i = 0;

  if ((n == 0) || ISHEADER(line_info[n - 1].type) ||
      (check_protected_header_marker(raw) == 0))
  {
    if (buf[0] == '\n') /* end of header */
    {
      line_info[n].type = MT_COLOR_NORMAL;
      getyx(stdscr, braille_line, braille_col);
    }
    else
    {
      /* if this is a continuation of the previous line, use the previous
       * line's color as default. */
      if ((n > 0) && ((buf[0] == ' ') || (buf[0] == '\t')))
      {
        line_info[n].type = line_info[n - 1].type; /* wrapped line */
        if (!C_HeaderColorPartial)
        {
          (line_info[n].syntax)[0].color = (line_info[n - 1].syntax)[0].color;
          line_info[n].is_cont_hdr = 1;
        }
      }
      else
      {
        line_info[n].type = MT_COLOR_HDEFAULT;
      }

      /* When this option is unset, we color the entire header the
       * same color.  Otherwise, we handle the header patterns just
       * like body patterns (further below).  */
      if (!C_HeaderColorPartial)
      {
        STAILQ_FOREACH(color_line, &ColorHdrList, entries)
        {
          if (regexec(&color_line->regex, buf, 0, NULL, 0) == 0)
          {
            line_info[n].type = MT_COLOR_HEADER;
            line_info[n].syntax[0].color = color_line->pair;
            if (line_info[n].is_cont_hdr)
            {
              /* adjust the previous continuation lines to reflect the color of this continuation line */
              int j;
              for (j = n - 1; j >= 0 && line_info[j].is_cont_hdr; --j)
              {
                line_info[j].type = line_info[n].type;
                line_info[j].syntax[0].color = line_info[n].syntax[0].color;
              }
              /* now adjust the first line of this header field */
              if (j >= 0)
              {
                line_info[j].type = line_info[n].type;
                line_info[j].syntax[0].color = line_info[n].syntax[0].color;
              }
              *force_redraw = true; /* the previous lines have already been drawn on the screen */
            }
            break;
          }
        }
      }
    }
  }
  else if (mutt_str_startswith(raw, "\033[0m", CASE_MATCH)) /* a little hack... */
    line_info[n].type = MT_COLOR_NORMAL;
  else if (check_attachment_marker((char *) raw) == 0)
    line_info[n].type = MT_COLOR_ATTACHMENT;
  else if ((mutt_str_strcmp("-- \n", buf) == 0) || (mutt_str_strcmp("-- \r\n", buf) == 0))
  {
    i = n + 1;

    line_info[n].type = MT_COLOR_SIGNATURE;
    while (i < last && check_sig(buf, line_info, i - 1) == 0 &&
           (line_info[i].type == MT_COLOR_NORMAL || line_info[i].type == MT_COLOR_QUOTED ||
            line_info[i].type == MT_COLOR_HEADER))
    {
      /* oops... */
      if (line_info[i].chunks)
      {
        line_info[i].chunks = 0;
        mutt_mem_realloc(&(line_info[n].syntax), sizeof(struct Syntax));
      }
      line_info[i++].type = MT_COLOR_SIGNATURE;
    }
  }
  else if (check_sig(buf, line_info, n - 1) == 0)
    line_info[n].type = MT_COLOR_SIGNATURE;
  else if (mutt_is_quote_line(buf, pmatch))

  {
    if (q_classify && (line_info[n].quote == NULL))
    {
      line_info[n].quote = classify_quote(quote_list, buf + pmatch[0].rm_so,
                                          pmatch[0].rm_eo - pmatch[0].rm_so,
                                          force_redraw, q_level);
    }
    line_info[n].type = MT_COLOR_QUOTED;
  }
  else
    line_info[n].type = MT_COLOR_NORMAL;

  /* body patterns */
  if ((line_info[n].type == MT_COLOR_NORMAL) || (line_info[n].type == MT_COLOR_QUOTED) ||
      ((line_info[n].type == MT_COLOR_HDEFAULT) && C_HeaderColorPartial))
  {
    size_t nl;

    /* don't consider line endings part of the buffer
     * for regex matching */
    nl = mutt_str_strlen(buf);
    if ((nl > 0) && (buf[nl - 1] == '\n'))
      buf[nl - 1] = '\0';

    i = 0;
    offset = 0;
    line_info[n].chunks = 0;
    if (line_info[n].type == MT_COLOR_HDEFAULT)
      head = &ColorHdrList;
    else
      head = &ColorBodyList;
    STAILQ_FOREACH(color_line, head, entries)
    {
      color_line->stop_matching = false;
    }
    do
    {
      if (!buf[offset])
        break;

      found = false;
      null_rx = false;
      STAILQ_FOREACH(color_line, head, entries)
      {
        if (!color_line->stop_matching && (regexec(&color_line->regex, buf + offset, 1, pmatch,
                                                   (offset ? REG_NOTBOL : 0)) == 0))
        {
          if (pmatch[0].rm_eo != pmatch[0].rm_so)
          {
            if (!found)
            {
              /* Abort if we fill up chunks.
               * Yes, this really happened. See #3888 */
              if (line_info[n].chunks == SHRT_MAX)
              {
                null_rx = false;
                break;
              }
              if (++(line_info[n].chunks) > 1)
              {
                mutt_mem_realloc(&(line_info[n].syntax),
                                 (line_info[n].chunks) * sizeof(struct Syntax));
              }
            }
            i = line_info[n].chunks - 1;
            pmatch[0].rm_so += offset;
            pmatch[0].rm_eo += offset;
            if (!found || (pmatch[0].rm_so < (line_info[n].syntax)[i].first) ||
                ((pmatch[0].rm_so == (line_info[n].syntax)[i].first) &&
                 (pmatch[0].rm_eo > (line_info[n].syntax)[i].last)))
            {
              (line_info[n].syntax)[i].color = color_line->pair;
              (line_info[n].syntax)[i].first = pmatch[0].rm_so;
              (line_info[n].syntax)[i].last = pmatch[0].rm_eo;
            }
            found = true;
            null_rx = false;
          }
          else
            null_rx = true; /* empty regex; don't add it, but keep looking */
        }
        else
        {
          /* Once a regexp fails to match, don't try matching it again.
           * On very long lines this can cause a performance issue if there
           * are other regexps that have many matches. */
          color_line->stop_matching = true;
        }
      }

      if (null_rx)
        offset++; /* avoid degenerate cases */
      else
        offset = (line_info[n].syntax)[i].last;
    } while (found || null_rx);
    if (nl > 0)
      buf[nl] = '\n';
  }

  /* attachment patterns */
  if (line_info[n].type == MT_COLOR_ATTACHMENT)
  {
    size_t nl;

    /* don't consider line endings part of the buffer for regex matching */
    nl = mutt_str_strlen(buf);
    if ((nl > 0) && (buf[nl - 1] == '\n'))
      buf[nl - 1] = '\0';

    i = 0;
    offset = 0;
    line_info[n].chunks = 0;
    do
    {
      if (!buf[offset])
        break;

      found = false;
      null_rx = false;
      STAILQ_FOREACH(color_line, &ColorAttachList, entries)
      {
        if (regexec(&color_line->regex, buf + offset, 1, pmatch,
                    (offset ? REG_NOTBOL : 0)) == 0)
        {
          if (pmatch[0].rm_eo != pmatch[0].rm_so)
          {
            if (!found)
            {
              if (++(line_info[n].chunks) > 1)
              {
                mutt_mem_realloc(&(line_info[n].syntax),
                                 (line_info[n].chunks) * sizeof(struct Syntax));
              }
            }
            i = line_info[n].chunks - 1;
            pmatch[0].rm_so += offset;
            pmatch[0].rm_eo += offset;
            if (!found || (pmatch[0].rm_so < (line_info[n].syntax)[i].first) ||
                ((pmatch[0].rm_so == (line_info[n].syntax)[i].first) &&
                 (pmatch[0].rm_eo > (line_info[n].syntax)[i].last)))
            {
              (line_info[n].syntax)[i].color = color_line->pair;
              (line_info[n].syntax)[i].first = pmatch[0].rm_so;
              (line_info[n].syntax)[i].last = pmatch[0].rm_eo;
            }
            found = 1;
            null_rx = 0;
          }
          else
            null_rx = 1; /* empty regex; don't add it, but keep looking */
        }
      }

      if (null_rx)
        offset++; /* avoid degenerate cases */
      else
        offset = (line_info[n].syntax)[i].last;
    } while (found || null_rx);
    if (nl > 0)
      buf[nl] = '\n';
  }
}

/**
 * is_ansi - Is this an ANSI escape sequence?
 * @param buf String to check
 * @retval true If it is
 */
static bool is_ansi(unsigned char *buf)
{
  while ((*buf != '\0') && (isdigit(*buf) || *buf == ';'))
    buf++;
  return *buf == 'm';
}

/**
 * grok_ansi - Parse an ANSI escape sequence
 * @param buf String to parse
 * @param pos Starting position in string
 * @param a   AnsiAttr for the result
 * @retval num Index of first character after the escape sequence
 */
static int grok_ansi(unsigned char *buf, int pos, struct AnsiAttr *a)
{
  int x = pos;

  while (isdigit(buf[x]) || buf[x] == ';')
    x++;

  /* Character Attributes */
  if (C_AllowAnsi && a && (buf[x] == 'm'))
  {
    if (pos == x)
    {
#ifdef HAVE_COLOR
      if (a->pair != -1)
        mutt_free_color(a->fg, a->bg);
#endif
      a->attr = ANSI_OFF;
      a->pair = -1;
    }
    while (pos < x)
    {
      if ((buf[pos] == '1') && ((pos + 1 == x) || (buf[pos + 1] == ';')))
      {
        a->attr |= ANSI_BOLD;
        pos += 2;
      }
      else if ((buf[pos] == '4') && ((pos + 1 == x) || (buf[pos + 1] == ';')))
      {
        a->attr |= ANSI_UNDERLINE;
        pos += 2;
      }
      else if ((buf[pos] == '5') && ((pos + 1 == x) || (buf[pos + 1] == ';')))
      {
        a->attr |= ANSI_BLINK;
        pos += 2;
      }
      else if ((buf[pos] == '7') && ((pos + 1 == x) || (buf[pos + 1] == ';')))
      {
        a->attr |= ANSI_REVERSE;
        pos += 2;
      }
      else if ((buf[pos] == '0') && ((pos + 1 == x) || (buf[pos + 1] == ';')))
      {
#ifdef HAVE_COLOR
        if (a->pair != -1)
          mutt_free_color(a->fg, a->bg);
#endif
        a->attr = ANSI_OFF;
        a->pair = -1;
        pos += 2;
      }
      else if ((buf[pos] == '3') && isdigit(buf[pos + 1]))
      {
#ifdef HAVE_COLOR
        if (a->pair != -1)
          mutt_free_color(a->fg, a->bg);
#endif
        a->pair = -1;
        a->attr |= ANSI_COLOR;
        a->fg = buf[pos + 1] - '0';
        pos += 3;
      }
      else if ((buf[pos] == '4') && isdigit(buf[pos + 1]))
      {
#ifdef HAVE_COLOR
        if (a->pair != -1)
          mutt_free_color(a->fg, a->bg);
#endif
        a->pair = -1;
        a->attr |= ANSI_COLOR;
        a->bg = buf[pos + 1] - '0';
        pos += 3;
      }
      else
      {
        while (pos < x && buf[pos] != ';')
          pos++;
        pos++;
      }
    }
  }
  pos = x;
  return pos;
}

/**
 * fill_buffer - Fill a buffer from a file
 * @param[in]     fp        File to read from
 * @param[in,out] last_pos  End of last read
 * @param[in]     offset    Position start reading from
 * @param[out]    buf       Buffer to fill
 * @param[out]    fmt       Copy of buffer, stripped of attributes
 * @param[out]    blen      Length of the buffer
 * @param[in,out] buf_ready true if the buffer already has data in it
 * @retval >=0 Bytes read
 * @retval -1  Error
 */
static int fill_buffer(FILE *fp, LOFF_T *last_pos, LOFF_T offset, unsigned char **buf,
                       unsigned char **fmt, size_t *blen, int *buf_ready)
{
  unsigned char *p = NULL, *q = NULL;
  static int b_read;
  int l = 0;

  if (*buf_ready == 0)
  {
    if (offset != *last_pos)
      fseeko(fp, offset, SEEK_SET);
    *buf = (unsigned char *) mutt_file_read_line((char *) *buf, blen, fp, &l, MUTT_EOL);
    if (!*buf)
    {
      fmt[0] = NULL;
      return -1;
    }
    *last_pos = ftello(fp);
    b_read = (int) (*last_pos - offset);
    *buf_ready = 1;

    mutt_mem_realloc(fmt, *blen);

    /* copy "buf" to "fmt", but without bold and underline controls */
    p = *buf;
    q = *fmt;
    while (*p)
    {
      if ((*p == '\010') && (p > *buf))
      {
        if (*(p + 1) == '_') /* underline */
          p += 2;
        else if (*(p + 1) && (q > *fmt)) /* bold or overstrike */
        {
          *(q - 1) = *(p + 1);
          p += 2;
        }
        else /* ^H */
          *q++ = *p++;
      }
      else if ((*p == '\033') && (*(p + 1) == '[') && is_ansi(p + 2))
      {
        while (*p++ != 'm') /* skip ANSI sequence */
          ;
      }
      else if ((*p == '\033') && (*(p + 1) == ']') &&
               ((check_attachment_marker((char *) p) == 0) ||
                (check_protected_header_marker((char *) p) == 0)))
      {
        mutt_debug(LL_DEBUG2, "Seen attachment marker.\n");
        while (*p++ != '\a') /* skip pseudo-ANSI sequence */
          ;
      }
      else
        *q++ = *p++;
    }
    *q = '\0';
  }
  return b_read;
}

/**
 * format_line - Display a line of text in the pager
 * @param[out] line_info    Line info
 * @param[in]  n            Line number (index into line_info)
 * @param[in]  buf          Text to display
 * @param[in]  flags        Flags, see #PagerFlags
 * @param[out] pa           ANSI attributes used
 * @param[in]  cnt          Length of text buffer
 * @param[out] pspace       Index of last whitespace character
 * @param[out] pvch         Number of bytes read
 * @param[out] pcol         Number of columns used
 * @param[out] pspecial     Attribute flags, e.g. A_UNDERLINE
 * @param[in]  pager_window Window to write to
 * @retval num Number of characters displayed
 */
static int format_line(struct Line **line_info, int n, unsigned char *buf,
                       PagerFlags flags, struct AnsiAttr *pa, int cnt, int *pspace,
                       int *pvch, int *pcol, int *pspecial, struct MuttWindow *pager_window)
{
  int space = -1; /* index of the last space or TAB */
  int col = C_Markers ? (*line_info)[n].continuation : 0;
  size_t k;
  int ch, vch, last_special = -1, special = 0, t;
  wchar_t wc;
  mbstate_t mbstate;
  int wrap_cols =
      mutt_window_wrap_cols(pager_window, (flags & MUTT_PAGER_NOWRAP) ? 0 : C_Wrap);

  if (check_attachment_marker((char *) buf) == 0)
    wrap_cols = pager_window->cols;

  /* FIXME: this should come from line_info */
  memset(&mbstate, 0, sizeof(mbstate));

  for (ch = 0, vch = 0; ch < cnt; ch += k, vch += k)
  {
    /* Handle ANSI sequences */
    while (cnt - ch >= 2 && buf[ch] == '\033' && buf[ch + 1] == '[' && is_ansi(buf + ch + 2))
      ch = grok_ansi(buf, ch + 2, pa) + 1;

    while (cnt - ch >= 2 && buf[ch] == '\033' && buf[ch + 1] == ']' &&
           ((check_attachment_marker((char *) buf + ch) == 0) ||
            (check_protected_header_marker((char *) buf + ch) == 0)))
    {
      while (buf[ch++] != '\a')
        if (ch >= cnt)
          break;
    }

    /* is anything left to do? */
    if (ch >= cnt)
      break;

    k = mbrtowc(&wc, (char *) buf + ch, cnt - ch, &mbstate);
    if ((k == (size_t)(-2)) || (k == (size_t)(-1)))
    {
      if (k == (size_t)(-1))
        memset(&mbstate, 0, sizeof(mbstate));
      mutt_debug(LL_DEBUG1, "mbrtowc returned %lu; errno = %d.\n", k, errno);
      if (col + 4 > wrap_cols)
        break;
      col += 4;
      if (pa)
        printw("\\%03o", buf[ch]);
      k = 1;
      continue;
    }
    if (k == 0)
      k = 1;

    if (CharsetIsUtf8)
    {
      /* zero width space, zero width no-break space */
      if ((wc == 0x200B) || (wc == 0xFEFF))
      {
        mutt_debug(LL_DEBUG3, "skip zero-width character U+%04X\n", (unsigned short) wc);
        continue;
      }
      if (mutt_mb_is_display_corrupting_utf8(wc))
      {
        mutt_debug(LL_DEBUG3, "filtered U+%04X\n", (unsigned short) wc);
        continue;
      }
    }

    /* Handle backspace */
    special = 0;
    if (IsWPrint(wc))
    {
      wchar_t wc1;
      mbstate_t mbstate1 = mbstate;
      size_t k1 = mbrtowc(&wc1, (char *) buf + ch + k, cnt - ch - k, &mbstate1);
      while ((k1 != (size_t)(-2)) && (k1 != (size_t)(-1)) && (k1 > 0) && (wc1 == '\b'))
      {
        const size_t k2 =
            mbrtowc(&wc1, (char *) buf + ch + k + k1, cnt - ch - k - k1, &mbstate1);
        if ((k2 == (size_t)(-2)) || (k2 == (size_t)(-1)) || (k2 == 0) || (!IsWPrint(wc1)))
          break;

        if (wc == wc1)
        {
          special |= (wc == '_' && special & A_UNDERLINE) ? A_UNDERLINE : A_BOLD;
        }
        else if ((wc == '_') || (wc1 == '_'))
        {
          special |= A_UNDERLINE;
          wc = (wc1 == '_') ? wc : wc1;
        }
        else
        {
          /* special = 0; / * overstrike: nothing to do! */
          wc = wc1;
        }

        ch += k + k1;
        k = k2;
        mbstate = mbstate1;
        k1 = mbrtowc(&wc1, (char *) buf + ch + k, cnt - ch - k, &mbstate1);
      }
    }

    if (pa && ((flags & (MUTT_SHOWCOLOR | MUTT_SEARCH | MUTT_PAGER_MARKER)) ||
               special || last_special || pa->attr))
    {
      resolve_color(*line_info, n, vch, flags, special, pa);
      last_special = special;
    }

    /* no-break space, narrow no-break space */
    if (IsWPrint(wc) || (CharsetIsUtf8 && ((wc == 0x00A0) || (wc == 0x202F))))
    {
      if (wc == ' ')
      {
        space = ch;
      }
      t = wcwidth(wc);
      if (col + t > wrap_cols)
        break;
      col += t;
      if (pa)
        mutt_addwch(wc);
    }
    else if (wc == '\n')
      break;
    else if (wc == '\t')
    {
      space = ch;
      t = (col & ~7) + 8;
      if (t > wrap_cols)
        break;
      if (pa)
        for (; col < t; col++)
          addch(' ');
      else
        col = t;
    }
    else if ((wc < 0x20) || (wc == 0x7f))
    {
      if (col + 2 > wrap_cols)
        break;
      col += 2;
      if (pa)
        printw("^%c", ('@' + wc) & 0x7f);
    }
    else if (wc < 0x100)
    {
      if (col + 4 > wrap_cols)
        break;
      col += 4;
      if (pa)
        printw("\\%03o", wc);
    }
    else
    {
      if (col + 1 > wrap_cols)
        break;
      col += k;
      if (pa)
        addch(ReplacementChar);
    }
  }
  *pspace = space;
  *pcol = col;
  *pvch = vch;
  *pspecial = special;
  return ch;
}

/**
 * display_line - Print a line on screen
 * @param[in]  fp              File to read from
 * @param[out] last_pos        Offset into file
 * @param[out] line_info       Line attributes
 * @param[in]  n               Line number
 * @param[out] last            Last line
 * @param[out] max             Maximum number of lines
 * @param[in]  flags           Flags, see #PagerFlags
 * @param[out] quote_list      Email quoting style
 * @param[out] q_level         Level of quoting
 * @param[out] force_redraw    Force a repaint
 * @param[out] search_re       Regex to highlight
 * @param[in]  pager_window    Window to draw into
 * @retval -1 EOF was reached
 * @retval 0  normal exit, line was not displayed
 * @retval >0 normal exit, line was displayed
 */
static int display_line(FILE *fp, LOFF_T *last_pos, struct Line **line_info,
                        int n, int *last, int *max, PagerFlags flags,
                        struct QClass **quote_list, int *q_level, bool *force_redraw,
                        regex_t *search_re, struct MuttWindow *pager_window)
{
  unsigned char *buf = NULL, *fmt = NULL;
  size_t buflen = 0;
  unsigned char *buf_ptr = NULL;
  int ch, vch, col, cnt, b_read;
  int buf_ready = 0;
  bool change_last = false;
  int special;
  int offset;
  int def_color;
  int m;
  int rc = -1;
  struct AnsiAttr a = { 0, 0, 0, -1 };
  regmatch_t pmatch[1];

  if (n == *last)
  {
    (*last)++;
    change_last = true;
  }

  if (*last == *max)
  {
    mutt_mem_realloc(line_info, sizeof(struct Line) * (*max += LINES));
    for (ch = *last; ch < *max; ch++)
    {
      memset(&((*line_info)[ch]), 0, sizeof(struct Line));
      (*line_info)[ch].type = -1;
      (*line_info)[ch].search_cnt = -1;
      (*line_info)[ch].syntax = mutt_mem_malloc(sizeof(struct Syntax));
      ((*line_info)[ch].syntax)[0].first = -1;
      ((*line_info)[ch].syntax)[0].last = -1;
    }
  }

  if (flags & MUTT_PAGER_LOGS)
  {
    /* determine the line class */
    if (fill_buffer(fp, last_pos, (*line_info)[n].offset, &buf, &fmt, &buflen, &buf_ready) < 0)
    {
      if (change_last)
        (*last)--;
      goto out;
    }

    (*line_info)[n].type = MT_COLOR_MESSAGE_LOG;
    if (buf[11] == 'M')
      (*line_info)[n].syntax[0].color = MT_COLOR_MESSAGE;
    else if (buf[11] == 'E')
      (*line_info)[n].syntax[0].color = MT_COLOR_ERROR;
    else
      (*line_info)[n].syntax[0].color = MT_COLOR_NORMAL;
  }

  /* only do color highlighting if we are viewing a message */
  if (flags & (MUTT_SHOWCOLOR | MUTT_TYPES))
  {
    if ((*line_info)[n].type == -1)
    {
      /* determine the line class */
      if (fill_buffer(fp, last_pos, (*line_info)[n].offset, &buf, &fmt, &buflen, &buf_ready) < 0)
      {
        if (change_last)
          (*last)--;
        goto out;
      }

      resolve_types((char *) fmt, (char *) buf, *line_info, n, *last,
                    quote_list, q_level, force_redraw, flags & MUTT_SHOWCOLOR);

      /* avoid race condition for continuation lines when scrolling up */
      for (m = n + 1; m < *last && (*line_info)[m].offset && (*line_info)[m].continuation; m++)
        (*line_info)[m].type = (*line_info)[n].type;
    }

    /* this also prevents searching through the hidden lines */
    if ((flags & MUTT_HIDE) && ((*line_info)[n].type == MT_COLOR_QUOTED))
      flags = 0; /* MUTT_NOSHOW */
  }

  /* At this point, (*line_info[n]).quote may still be undefined. We
   * don't want to compute it every time MUTT_TYPES is set, since this
   * would slow down the "bottom" function unacceptably. A compromise
   * solution is hence to call regexec() again, just to find out the
   * length of the quote prefix.  */
  if ((flags & MUTT_SHOWCOLOR) && !(*line_info)[n].continuation &&
      ((*line_info)[n].type == MT_COLOR_QUOTED) && !(*line_info)[n].quote)
  {
    if (fill_buffer(fp, last_pos, (*line_info)[n].offset, &buf, &fmt, &buflen, &buf_ready) < 0)
    {
      if (change_last)
        (*last)--;
      goto out;
    }
    if (C_QuoteRegex && C_QuoteRegex->regex &&
        (regexec(C_QuoteRegex->regex, (char *) fmt, 1, pmatch, 0) == 0))
    {
      (*line_info)[n].quote =
          classify_quote(quote_list, (char *) fmt + pmatch[0].rm_so,
                         pmatch[0].rm_eo - pmatch[0].rm_so, force_redraw, q_level);
    }
    else
    {
      goto out;
    }
  }

  if ((flags & MUTT_SEARCH) && !(*line_info)[n].continuation &&
      ((*line_info)[n].search_cnt == -1))
  {
    if (fill_buffer(fp, last_pos, (*line_info)[n].offset, &buf, &fmt, &buflen, &buf_ready) < 0)
    {
      if (change_last)
        (*last)--;
      goto out;
    }

    offset = 0;
    (*line_info)[n].search_cnt = 0;
    while (regexec(search_re, (char *) fmt + offset, 1, pmatch,
                   (offset ? REG_NOTBOL : 0)) == 0)
    {
      if (++((*line_info)[n].search_cnt) > 1)
      {
        mutt_mem_realloc(&((*line_info)[n].search),
                         ((*line_info)[n].search_cnt) * sizeof(struct Syntax));
      }
      else
        (*line_info)[n].search = mutt_mem_malloc(sizeof(struct Syntax));
      pmatch[0].rm_so += offset;
      pmatch[0].rm_eo += offset;
      ((*line_info)[n].search)[(*line_info)[n].search_cnt - 1].first = pmatch[0].rm_so;
      ((*line_info)[n].search)[(*line_info)[n].search_cnt - 1].last = pmatch[0].rm_eo;

      if (pmatch[0].rm_eo == pmatch[0].rm_so)
        offset++; /* avoid degenerate cases */
      else
        offset = pmatch[0].rm_eo;
      if (!fmt[offset])
        break;
    }
  }

  if (!(flags & MUTT_SHOW) && ((*line_info)[n + 1].offset > 0))
  {
    /* we've already scanned this line, so just exit */
    rc = 0;
    goto out;
  }
  if ((flags & MUTT_SHOWCOLOR) && *force_redraw && ((*line_info)[n + 1].offset > 0))
  {
    /* no need to try to display this line... */
    rc = 1;
    goto out; /* fake display */
  }

  b_read = fill_buffer(fp, last_pos, (*line_info)[n].offset, &buf, &fmt, &buflen, &buf_ready);
  if (b_read < 0)
  {
    if (change_last)
      (*last)--;
    goto out;
  }

  /* now chose a good place to break the line */
  cnt = format_line(line_info, n, buf, flags, 0, b_read, &ch, &vch, &col, &special, pager_window);
  buf_ptr = buf + cnt;

  /* move the break point only if smart_wrap is set */
  if (C_SmartWrap)
  {
    if ((cnt < b_read) && (ch != -1) && !ISHEADER((*line_info)[n].type) &&
        !ISSPACE(buf[cnt]))
    {
      buf_ptr = buf + ch;
      /* skip trailing blanks */
      while (ch && (buf[ch] == ' ' || buf[ch] == '\t' || buf[ch] == '\r'))
        ch--;
      /* A very long word with leading spaces causes infinite
       * wrapping when MUTT_PAGER_NSKIP is set.  A folded header
       * with a single long word shouldn't be smartwrapped
       * either.  So just disable smart_wrap if it would wrap at the
       * beginning of the line. */
      if (!ch)
        buf_ptr = buf + cnt;
      else
        cnt = ch + 1;
    }
    if (!(flags & MUTT_PAGER_NSKIP))
    {
      /* skip leading blanks on the next line too */
      while (*buf_ptr == ' ' || *buf_ptr == '\t')
        buf_ptr++;
    }
  }

  if (*buf_ptr == '\r')
    buf_ptr++;
  if (*buf_ptr == '\n')
    buf_ptr++;

  if (((int) (buf_ptr - buf) < b_read) && !(*line_info)[n + 1].continuation)
    append_line(*line_info, n, (int) (buf_ptr - buf));
  (*line_info)[n + 1].offset = (*line_info)[n].offset + (long) (buf_ptr - buf);

  /* if we don't need to display the line we are done */
  if (!(flags & MUTT_SHOW))
  {
    rc = 0;
    goto out;
  }

  /* display the line */
  format_line(line_info, n, buf, flags, &a, cnt, &ch, &vch, &col, &special, pager_window);

/* avoid a bug in ncurses... */
#ifndef USE_SLANG_CURSES
  if (col == 0)
  {
    NORMAL_COLOR;
    addch(' ');
  }
#endif

  /* end the last color pattern (needed by S-Lang) */
  if (special || ((col != pager_window->cols) && (flags & (MUTT_SHOWCOLOR | MUTT_SEARCH))))
    resolve_color(*line_info, n, vch, flags, 0, &a);

  /* Fill the blank space at the end of the line with the prevailing color.
   * ncurses does an implicit clrtoeol() when you do addch('\n') so we have
   * to make sure to reset the color *after* that */
  if (flags & MUTT_SHOWCOLOR)
  {
    m = ((*line_info)[n].continuation) ? ((*line_info)[n].syntax)[0].first : n;
    if ((*line_info)[m].type == MT_COLOR_HEADER)
      def_color = ((*line_info)[m].syntax)[0].color;
    else
      def_color = ColorDefs[(*line_info)[m].type];

    ATTRSET(def_color);
  }

  if (col < pager_window->cols)
    mutt_window_clrtoeol(pager_window);

  /* reset the color back to normal.  This *must* come after the
   * clrtoeol, otherwise the color for this line will not be
   * filled to the right margin.  */
  if (flags & MUTT_SHOWCOLOR)
    NORMAL_COLOR;

  /* build a return code */
  if (!(flags & MUTT_SHOW))
    flags = 0;

  rc = flags;

out:
  FREE(&buf);
  FREE(&fmt);
  return rc;
}

/**
 * up_n_lines - Reposition the pager's view up by n lines
 * @param nlines Number of lines to move
 * @param info   Line info array
 * @param cur    Current line number
 * @param hiding true if lines have been hidden
 * @retval num New current line number
 */
static int up_n_lines(int nlines, struct Line *info, int cur, bool hiding)
{
  while (cur > 0 && nlines > 0)
  {
    cur--;
    if (!hiding || (info[cur].type != MT_COLOR_QUOTED))
      nlines--;
  }

  return cur;
}

static const struct Mapping PagerHelp[] = {
  { N_("Exit"), OP_EXIT },
  { N_("PrevPg"), OP_PREV_PAGE },
  { N_("NextPg"), OP_NEXT_PAGE },
  { NULL, 0 },
};

static const struct Mapping PagerHelpExtra[] = {
  { N_("View Attachm."), OP_VIEW_ATTACHMENTS },
  { N_("Del"), OP_DELETE },
  { N_("Reply"), OP_REPLY },
  { N_("Next"), OP_MAIN_NEXT_UNDELETED },
  { NULL, 0 },
};

#ifdef USE_NNTP
static struct Mapping PagerNewsHelpExtra[] = {
  { N_("Post"), OP_POST },
  { N_("Followup"), OP_FOLLOWUP },
  { N_("Del"), OP_DELETE },
  { N_("Next"), OP_MAIN_NEXT_UNDELETED },
  { NULL, 0 },
};
#endif

/**
 * mutt_clear_pager_position - Reset the pager's viewing position
 */
void mutt_clear_pager_position(void)
{
  TopLine = 0;
  OldHdr = NULL;
}

/**
 * struct PagerRedrawData - Keep track when the pager needs redrawing
 */
struct PagerRedrawData
{
  PagerFlags flags;
  struct Pager *extra;
  int indexlen;
  int indicator; /**< the indicator line of the PI */
  int oldtopline;
  int lines;
  int max_line;
  int last_line;
  int curline;
  int topline;
  bool force_redraw;
  int has_types;
  PagerFlags hide_quoted;
  int q_level;
  struct QClass *quote_list;
  LOFF_T last_pos;
  LOFF_T last_offset;
  struct MuttWindow *index_status_window;
  struct MuttWindow *index_window;
  struct MuttWindow *pager_status_window;
  struct MuttWindow *pager_window;
  struct Menu *index; /**< the Pager Index (PI) */
  regex_t search_re;
  bool search_compiled;
  PagerFlags search_flag;
  bool search_back;
  const char *banner;
  char *helpstr;
  char *searchbuf;
  struct Line *line_info;
  FILE *fp;
  struct stat sb;
};

/**
 * pager_custom_redraw - Redraw the pager window - Implements Menu::menu_custom_redraw()
 */
static void pager_custom_redraw(struct Menu *pager_menu)
{
  struct PagerRedrawData *rd = pager_menu->redraw_data;
  char buf[1024];

  if (!rd)
    return;

  if (pager_menu->redraw & REDRAW_FULL)
  {
    mutt_window_reflow();
    NORMAL_COLOR;
    /* clear() doesn't optimize screen redraws */
    move(0, 0);
    clrtobot();

    if (IsEmail(rd->extra) && Context && ((Context->mailbox->vcount + 1) < C_PagerIndexLines))
      rd->indexlen = Context->mailbox->vcount + 1;
    else
      rd->indexlen = C_PagerIndexLines;

    rd->indicator = rd->indexlen / 3;

    memcpy(rd->pager_window, MuttIndexWindow, sizeof(struct MuttWindow));
    memcpy(rd->pager_status_window, MuttStatusWindow, sizeof(struct MuttWindow));
    rd->index_status_window->rows = 0;
    rd->index_window->rows = 0;

    if (IsEmail(rd->extra) && C_PagerIndexLines)
    {
      memcpy(rd->index_window, MuttIndexWindow, sizeof(struct MuttWindow));
      rd->index_window->rows = rd->indexlen > 0 ? rd->indexlen - 1 : 0;

      if (C_StatusOnTop)
      {
        memcpy(rd->index_status_window, MuttStatusWindow, sizeof(struct MuttWindow));

        memcpy(rd->pager_status_window, MuttIndexWindow, sizeof(struct MuttWindow));
        rd->pager_status_window->rows = 1;
        rd->pager_status_window->row_offset += rd->index_window->rows;

        rd->pager_window->rows -=
            rd->index_window->rows + rd->pager_status_window->rows;
        rd->pager_window->row_offset +=
            rd->index_window->rows + rd->pager_status_window->rows;
      }
      else
      {
        memcpy(rd->index_status_window, MuttIndexWindow, sizeof(struct MuttWindow));
        rd->index_status_window->rows = 1;
        rd->index_status_window->row_offset += rd->index_window->rows;

        rd->pager_window->rows -=
            rd->index_window->rows + rd->index_status_window->rows;
        rd->pager_window->row_offset +=
            rd->index_window->rows + rd->index_status_window->rows;
      }
    }

    if (C_Help)
    {
      SETCOLOR(MT_COLOR_STATUS);
      mutt_window_move(MuttHelpWindow, 0, 0);
      mutt_paddstr(MuttHelpWindow->cols, rd->helpstr);
      NORMAL_COLOR;
    }

    if (Resize)
    {
      rd->search_compiled = Resize->search_compiled;
      if (rd->search_compiled)
      {
        int flags = mutt_mb_is_lower(rd->searchbuf) ? REG_ICASE : 0;
        const int err = REGCOMP(&rd->search_re, rd->searchbuf, REG_NEWLINE | flags);
        if (err != 0)
        {
          regerror(err, &rd->search_re, buf, sizeof(buf));
          mutt_error("%s", buf);
          rd->search_compiled = false;
        }
        else
        {
          rd->search_flag = MUTT_SEARCH;
          rd->search_back = Resize->search_back;
        }
      }
      rd->lines = Resize->line;
      pager_menu->redraw |= REDRAW_FLOW;

      FREE(&Resize);
    }

    if (IsEmail(rd->extra) && C_PagerIndexLines)
    {
      if (!rd->index)
      {
        /* only allocate the space if/when we need the index.
         * Initialise the menu as per the main index */
        rd->index = mutt_menu_new(MENU_MAIN);
        rd->index->menu_make_entry = index_make_entry;
        rd->index->menu_color = index_color;
        rd->index->max = Context ? Context->mailbox->vcount : 0;
        rd->index->current = rd->extra->email->virtual;
        rd->index->indexwin = rd->index_window;
        rd->index->statuswin = rd->index_status_window;
      }

      NORMAL_COLOR;
      rd->index->pagelen = rd->index_window->rows;

      /* some fudge to work out whereabouts the indicator should go */
      if (rd->index->current - rd->indicator < 0)
        rd->index->top = 0;
      else if (rd->index->max - rd->index->current < rd->index->pagelen - rd->indicator)
        rd->index->top = rd->index->max - rd->index->pagelen;
      else
        rd->index->top = rd->index->current - rd->indicator;

      menu_redraw_index(rd->index);
    }

    pager_menu->redraw |= REDRAW_BODY | REDRAW_INDEX | REDRAW_STATUS;
#ifdef USE_SIDEBAR
    pager_menu->redraw |= REDRAW_SIDEBAR;
#endif
    mutt_show_error();
  }

  if (pager_menu->redraw & REDRAW_FLOW)
  {
    if (!(rd->flags & MUTT_PAGER_RETWINCH))
    {
      rd->lines = -1;
      for (int i = 0; i <= rd->topline; i++)
        if (!rd->line_info[i].continuation)
          rd->lines++;
      for (int i = 0; i < rd->max_line; i++)
      {
        rd->line_info[i].offset = 0;
        rd->line_info[i].type = -1;
        rd->line_info[i].continuation = 0;
        rd->line_info[i].chunks = 0;
        rd->line_info[i].search_cnt = -1;
        rd->line_info[i].quote = NULL;

        mutt_mem_realloc(&(rd->line_info[i].syntax), sizeof(struct Syntax));
        if (rd->search_compiled && rd->line_info[i].search)
          FREE(&(rd->line_info[i].search));
      }

      rd->last_line = 0;
      rd->topline = 0;
    }
    int i = -1;
    int j = -1;
    while (display_line(rd->fp, &rd->last_pos, &rd->line_info, ++i, &rd->last_line,
                        &rd->max_line, rd->has_types | rd->search_flag | (rd->flags & MUTT_PAGER_NOWRAP),
                        &rd->quote_list, &rd->q_level, &rd->force_redraw,
                        &rd->search_re, rd->pager_window) == 0)
    {
      if (!rd->line_info[i].continuation && (++j == rd->lines))
      {
        rd->topline = i;
        if (!rd->search_flag)
          break;
      }
    }
  }

#ifdef USE_SIDEBAR
  if (pager_menu->redraw & REDRAW_SIDEBAR)
  {
    menu_redraw_sidebar(pager_menu);
  }
#endif

  if ((pager_menu->redraw & REDRAW_BODY) || (rd->topline != rd->oldtopline))
  {
    do
    {
      mutt_window_move(rd->pager_window, 0, 0);
      rd->curline = rd->topline;
      rd->oldtopline = rd->topline;
      rd->lines = 0;
      rd->force_redraw = false;

      while (rd->lines < rd->pager_window->rows &&
             rd->line_info[rd->curline].offset <= rd->sb.st_size - 1)
      {
        if (display_line(rd->fp, &rd->last_pos, &rd->line_info, rd->curline,
                         &rd->last_line, &rd->max_line,
                         (rd->flags & MUTT_DISPLAYFLAGS) | rd->hide_quoted |
                             rd->search_flag | (rd->flags & MUTT_PAGER_NOWRAP),
                         &rd->quote_list, &rd->q_level, &rd->force_redraw,
                         &rd->search_re, rd->pager_window) > 0)
        {
          rd->lines++;
        }
        rd->curline++;
        mutt_window_move(rd->pager_window, rd->lines, 0);
      }
      rd->last_offset = rd->line_info[rd->curline].offset;
    } while (rd->force_redraw);

    SETCOLOR(MT_COLOR_TILDE);
    while (rd->lines < rd->pager_window->rows)
    {
      mutt_window_clrtoeol(rd->pager_window);
      if (C_Tilde)
        addch('~');
      rd->lines++;
      mutt_window_move(rd->pager_window, rd->lines, 0);
    }
    NORMAL_COLOR;

    /* We are going to update the pager status bar, so it isn't
     * necessary to reset to normal color now. */

    pager_menu->redraw |= REDRAW_STATUS; /* need to update the % seen */
  }

  if (pager_menu->redraw & REDRAW_STATUS)
  {
    struct HdrFormatInfo hfi;
    char pager_progress_str[65]; /* Lots of space for translations */

    hfi.ctx = Context;
    hfi.mailbox = Context ? Context->mailbox : NULL;
    hfi.pager_progress = pager_progress_str;

    if (rd->last_pos < rd->sb.st_size - 1)
    {
      snprintf(pager_progress_str, sizeof(pager_progress_str), OFF_T_FMT "%%",
               (100 * rd->last_offset / rd->sb.st_size));
    }
    else
    {
      const char *msg = (rd->topline == 0) ?
                            /* L10N: Status bar message: the entire email is visible in the pager */
                            _("all") :
                            /* L10N: Status bar message: the end of the email is visible in the pager */
                            _("end");
      mutt_str_strfcpy(pager_progress_str, msg, sizeof(pager_progress_str));
    }

    /* print out the pager status bar */
    mutt_window_move(rd->pager_status_window, 0, 0);
    SETCOLOR(MT_COLOR_STATUS);

    if (IsEmail(rd->extra) || IsMsgAttach(rd->extra))
    {
      size_t l1 = rd->pager_status_window->cols * MB_LEN_MAX;
      size_t l2 = sizeof(buf);
      hfi.email = (IsEmail(rd->extra)) ? rd->extra->email : rd->extra->body->email;
      mutt_make_string_info(buf, l1 < l2 ? l1 : l2, rd->pager_status_window->cols,
                            NONULL(C_PagerFormat), &hfi, 0);
      mutt_draw_statusline(rd->pager_status_window->cols, buf, l2);
    }
    else
    {
      char bn[256];
      snprintf(bn, sizeof(bn), "%s (%s)", rd->banner, pager_progress_str);
      mutt_draw_statusline(rd->pager_status_window->cols, bn, sizeof(bn));
    }
    NORMAL_COLOR;
    if (C_TsEnabled && TsSupported && rd->index)
    {
      menu_status_line(buf, sizeof(buf), rd->index, NONULL(C_TsStatusFormat));
      mutt_ts_status(buf);
      menu_status_line(buf, sizeof(buf), rd->index, NONULL(C_TsIconFormat));
      mutt_ts_icon(buf);
    }
  }

  if ((pager_menu->redraw & REDRAW_INDEX) && rd->index)
  {
    /* redraw the pager_index indicator, because the
     * flags for this message might have changed. */
    if (rd->index_window->rows > 0)
      menu_redraw_current(rd->index);

    /* print out the index status bar */
    menu_status_line(buf, sizeof(buf), rd->index, NONULL(C_StatusFormat));

    mutt_window_move(rd->index_status_window, 0, 0);
    SETCOLOR(MT_COLOR_STATUS);
    mutt_draw_statusline(rd->index_status_window->cols, buf, sizeof(buf));
    NORMAL_COLOR;
  }

  pager_menu->redraw = 0;
}

/**
 * mutt_pager - Display a file, or help, in a window
 * @param banner Title to display in status bar
 * @param fname  Name of file to read
 * @param flags  Flags, e.g. #MUTT_SHOWCOLOR
 * @param extra  Info about email to display
 * @retval  0 Success
 * @retval -1 Error
 *
 * This pager is actually not so simple as it once was.  It now operates in two
 * modes: one for viewing messages and the other for viewing help.  These can
 * be distinguished by whether or not "hdr" is NULL.  The "hdr" arg is
 * there so that we can do operations on the current message without the need
 * to pop back out to the main-menu.
 */
int mutt_pager(const char *banner, const char *fname, PagerFlags flags, struct Pager *extra)
{
  static char searchbuf[256] = "";
  char buf[1024];
  char helpstr[256];
  char tmphelp[256];
  int ch = 0, rc = -1;
  bool first = true;
  int searchctx = 0;
  bool wrapped = false;

  struct Menu *pager_menu = NULL;
  int old_PagerIndexLines; /* some people want to resize it while inside the pager */
#ifdef USE_NNTP
  char *followup_to = NULL;
#endif

  if (!(flags & MUTT_SHOWCOLOR))
    flags |= MUTT_SHOWFLAT;

  struct PagerRedrawData rd = { 0 };
  rd.banner = banner;
  rd.flags = flags;
  rd.extra = extra;
  rd.indexlen = C_PagerIndexLines;
  rd.indicator = rd.indexlen / 3;
  rd.helpstr = helpstr;
  rd.searchbuf = searchbuf;
  rd.has_types = (IsEmail(extra) || (flags & MUTT_SHOWCOLOR)) ? MUTT_TYPES : 0; /* main message or rfc822 attachment */

  rd.fp = fopen(fname, "r");
  if (!rd.fp)
  {
    mutt_perror(fname);
    return -1;
  }

  if (stat(fname, &rd.sb) != 0)
  {
    mutt_perror(fname);
    mutt_file_fclose(&rd.fp);
    return -1;
  }
  unlink(fname);

  /* Initialize variables */

  if (Context && IsEmail(extra) && !extra->email->read)
  {
    Context->msgnotreadyet = extra->email->msgno;
    mutt_set_flag(Context->mailbox, extra->email, MUTT_READ, true);
  }

  rd.max_line = LINES; /* number of lines on screen, from curses */
  rd.line_info = mutt_mem_calloc(rd.max_line, sizeof(struct Line));
  for (size_t i = 0; i < rd.max_line; i++)
  {
    rd.line_info[i].type = -1;
    rd.line_info[i].search_cnt = -1;
    rd.line_info[i].syntax = mutt_mem_malloc(sizeof(struct Syntax));
    (rd.line_info[i].syntax)[0].first = -1;
    (rd.line_info[i].syntax)[0].last = -1;
  }

  mutt_compile_help(helpstr, sizeof(helpstr), MENU_PAGER, PagerHelp);
  if (IsEmail(extra))
  {
    mutt_str_strfcpy(tmphelp, helpstr, sizeof(tmphelp));
    mutt_compile_help(buf, sizeof(buf), MENU_PAGER,
#ifdef USE_NNTP
                      (Context && (Context->mailbox->magic == MUTT_NNTP)) ?
                          PagerNewsHelpExtra :
#endif
                          PagerHelpExtra);
    snprintf(helpstr, sizeof(helpstr), "%s %s", tmphelp, buf);
  }
  if (!InHelp)
  {
    mutt_str_strfcpy(tmphelp, helpstr, sizeof(tmphelp));
    mutt_make_help(buf, sizeof(buf), _("Help"), MENU_PAGER, OP_HELP);
    snprintf(helpstr, sizeof(helpstr), "%s %s", tmphelp, buf);
  }

  rd.index_status_window = mutt_mem_calloc(1, sizeof(struct MuttWindow));
  rd.index_window = mutt_mem_calloc(1, sizeof(struct MuttWindow));
  rd.pager_status_window = mutt_mem_calloc(1, sizeof(struct MuttWindow));
  rd.pager_window = mutt_mem_calloc(1, sizeof(struct MuttWindow));

  pager_menu = mutt_menu_new(MENU_PAGER);
  pager_menu->menu_custom_redraw = pager_custom_redraw;
  pager_menu->redraw_data = &rd;
  mutt_menu_push_current(pager_menu);

  while (ch != -1)
  {
    mutt_curs_set(0);

    pager_custom_redraw(pager_menu);

    if (C_BrailleFriendly)
    {
      if (braille_line != -1)
      {
        move(braille_line + 1, 0);
        braille_line = -1;
      }
    }
    else
      mutt_window_move(rd.pager_status_window, 0, rd.pager_status_window->cols - 1);

    mutt_refresh();

    if (IsEmail(extra) && (OldHdr == extra->email) && (TopLine != rd.topline) &&
        (rd.line_info[rd.curline].offset < (rd.sb.st_size - 1)))
    {
      if ((TopLine - rd.topline) > rd.lines)
        rd.topline += rd.lines;
      else
        rd.topline = TopLine;
      continue;
    }
    else
      OldHdr = NULL;

    ch = km_dokey(MENU_PAGER);
    if (ch >= 0)
    {
      mutt_clear_error();
    }
    mutt_curs_set(1);

    bool do_new_mail = false;

    if (Context && Context->mailbox && !OptAttachMsg)
    {
      int index_hint = 0; /* used to restore cursor position */
      int oldcount = Context->mailbox->msg_count;
      /* check for new mail */
      int check = mx_mbox_check(Context->mailbox, &index_hint);
      if (check < 0)
      {
        if (!Context->mailbox || (Context->mailbox->path[0] == '\0'))
        {
          /* fatal error occurred */
          ctx_free(&Context);
          pager_menu->redraw = REDRAW_FULL;
          break;
        }
      }
      else if ((check == MUTT_NEW_MAIL) || (check == MUTT_REOPENED) || (check == MUTT_FLAGS))
      {
        /* notify user of newly arrived mail */
        if (check == MUTT_NEW_MAIL)
        {
          for (size_t i = oldcount; i < Context->mailbox->msg_count; i++)
          {
            struct Email *e = Context->mailbox->emails[i];

            if (e && !e->read)
            {
              mutt_message(_("New mail in this mailbox"));
              do_new_mail = true;
              break;
            }
          }
        }

        if ((check == MUTT_NEW_MAIL) || (check == MUTT_REOPENED))
        {
          if (rd.index && Context)
          {
            /* After the mailbox has been updated,
             * rd.index->current might be invalid */
            rd.index->current =
                MIN(rd.index->current, MAX(Context->mailbox->msg_count - 1, 0));
            index_hint = Context->mailbox
                             ->emails[Context->mailbox->v2r[rd.index->current]]
                             ->index;

            bool q = Context->mailbox->quiet;
            Context->mailbox->quiet = true;
            update_index(rd.index, Context, check, oldcount, index_hint);
            Context->mailbox->quiet = q;

            rd.index->max = Context->mailbox->vcount;

            /* If these header pointers don't match, then our email may have
             * been deleted.  Make the pointer safe, then leave the pager.
             * This have a unpleasant behaviour to close the pager even the
             * deleted message is not the opened one, but at least it's safe. */
            if (extra->email !=
                Context->mailbox->emails[Context->mailbox->v2r[rd.index->current]])
            {
              extra->email =
                  Context->mailbox->emails[Context->mailbox->v2r[rd.index->current]];
              break;
            }
          }

          pager_menu->redraw = REDRAW_FULL;
          OptSearchInvalid = true;
        }
      }

      if (mutt_mailbox_notify(Context ? Context->mailbox : NULL) || do_new_mail)
      {
        if (C_BeepNew)
          beep();
        if (C_NewMailCommand)
        {
          char cmd[1024];
          menu_status_line(cmd, sizeof(cmd), rd.index, NONULL(C_NewMailCommand));
          if (mutt_system(cmd) != 0)
            mutt_error(_("Error running \"%s\""), cmd);
        }
      }
    }

    if (SigWinch)
    {
      SigWinch = 0;
      mutt_resize_screen();
      clearok(stdscr, TRUE); /* force complete redraw */

      if (flags & MUTT_PAGER_RETWINCH)
      {
        /* Store current position. */
        rd.lines = -1;
        for (size_t i = 0; i <= rd.topline; i++)
          if (!rd.line_info[i].continuation)
            rd.lines++;

        Resize = mutt_mem_malloc(sizeof(struct Resize));

        Resize->line = rd.lines;
        Resize->search_compiled = rd.search_compiled;
        Resize->search_back = rd.search_back;

        ch = -1;
        rc = OP_REFORMAT_WINCH;
      }
      else
      {
        /* note: mutt_resize_screen() -> mutt_window_reflow() sets
         * REDRAW_FULL and REDRAW_FLOW */
        ch = 0;
      }
      continue;
    }

    if (ch < 0)
    {
      ch = 0;
      mutt_timeout_hook();
      continue;
    }

    rc = ch;

    switch (ch)
    {
      case OP_EXIT:
        rc = -1;
        ch = -1;
        break;

      case OP_QUIT:
        if (query_quadoption(C_Quit, _("Quit NeoMutt?")) == MUTT_YES)
        {
          /* avoid prompting again in the index menu */
          cs_str_native_set(Config, "quit", MUTT_YES, NULL);
          ch = -1;
        }
        break;

      case OP_NEXT_PAGE:
        if (rd.line_info[rd.curline].offset < (rd.sb.st_size - 1))
        {
          rd.topline = up_n_lines(C_PagerContext, rd.line_info, rd.curline, rd.hide_quoted);
        }
        else if (C_PagerStop)
        {
          /* emulate "less -q" and don't go on to the next message. */
          mutt_error(_("Bottom of message is shown"));
        }
        else
        {
          /* end of the current message, so display the next message. */
          rc = OP_MAIN_NEXT_UNDELETED;
          ch = -1;
        }
        break;

      case OP_PREV_PAGE:
        if (rd.topline != 0)
        {
          rd.topline = up_n_lines(rd.pager_window->rows - C_PagerContext,
                                  rd.line_info, rd.topline, rd.hide_quoted);
        }
        else
          mutt_message(_("Top of message is shown"));
        break;

      case OP_NEXT_LINE:
        if (rd.line_info[rd.curline].offset < (rd.sb.st_size - 1))
        {
          rd.topline++;
          if (rd.hide_quoted)
          {
            while ((rd.line_info[rd.topline].type == MT_COLOR_QUOTED) &&
                   (rd.topline < rd.last_line))
            {
              rd.topline++;
            }
          }
        }
        else
          mutt_message(_("Bottom of message is shown"));
        break;

      case OP_PREV_LINE:
        if (rd.topline)
          rd.topline = up_n_lines(1, rd.line_info, rd.topline, rd.hide_quoted);
        else
          mutt_error(_("Top of message is shown"));
        break;

      case OP_PAGER_TOP:
        if (rd.topline)
          rd.topline = 0;
        else
          mutt_error(_("Top of message is shown"));
        break;

      case OP_HALF_UP:
        if (rd.topline)
        {
          rd.topline = up_n_lines(rd.pager_window->rows / 2, rd.line_info,
                                  rd.topline, rd.hide_quoted);
        }
        else
          mutt_error(_("Top of message is shown"));
        break;

      case OP_HALF_DOWN:
        if (rd.line_info[rd.curline].offset < (rd.sb.st_size - 1))
        {
          rd.topline = up_n_lines(rd.pager_window->rows / 2, rd.line_info,
                                  rd.curline, rd.hide_quoted);
        }
        else if (C_PagerStop)
        {
          /* emulate "less -q" and don't go on to the next message. */
          mutt_error(_("Bottom of message is shown"));
        }
        else
        {
          /* end of the current message, so display the next message. */
          rc = OP_MAIN_NEXT_UNDELETED;
          ch = -1;
        }
        break;

      case OP_SEARCH_NEXT:
      case OP_SEARCH_OPPOSITE:
        if (rd.search_compiled)
        {
          wrapped = false;

          if (C_SearchContext < rd.pager_window->rows)
            searchctx = C_SearchContext;
          else
            searchctx = 0;

        search_next:
          if ((!rd.search_back && (ch == OP_SEARCH_NEXT)) ||
              (rd.search_back && (ch == OP_SEARCH_OPPOSITE)))
          {
            /* searching forward */
            int i;
            for (i = wrapped ? 0 : rd.topline + searchctx + 1; i < rd.last_line; i++)
            {
              if ((!rd.hide_quoted || (rd.line_info[i].type != MT_COLOR_QUOTED)) &&
                  !rd.line_info[i].continuation && (rd.line_info[i].search_cnt > 0))
              {
                break;
              }
            }

            if (i < rd.last_line)
              rd.topline = i;
            else if (wrapped || !C_WrapSearch)
              mutt_error(_("Not found"));
            else
            {
              mutt_message(_("Search wrapped to top"));
              wrapped = true;
              goto search_next;
            }
          }
          else
          {
            /* searching backward */
            int i;
            for (i = wrapped ? rd.last_line : rd.topline + searchctx - 1; i >= 0; i--)
            {
              if ((!rd.hide_quoted ||
                   (rd.has_types && (rd.line_info[i].type != MT_COLOR_QUOTED))) &&
                  !rd.line_info[i].continuation && (rd.line_info[i].search_cnt > 0))
              {
                break;
              }
            }

            if (i >= 0)
              rd.topline = i;
            else if (wrapped || !C_WrapSearch)
              mutt_error(_("Not found"));
            else
            {
              mutt_message(_("Search wrapped to bottom"));
              wrapped = true;
              goto search_next;
            }
          }

          if (rd.line_info[rd.topline].search_cnt > 0)
          {
            rd.search_flag = MUTT_SEARCH;
            /* give some context for search results */
            if (rd.topline - searchctx > 0)
              rd.topline -= searchctx;
          }

          break;
        }
        /* no previous search pattern */
        /* fallthrough */

      case OP_SEARCH:
      case OP_SEARCH_REVERSE:
        mutt_str_strfcpy(buf, searchbuf, sizeof(buf));
        if (mutt_get_field(((ch == OP_SEARCH) || (ch == OP_SEARCH_NEXT)) ?
                               _("Search for: ") :
                               _("Reverse search for: "),
                           buf, sizeof(buf), MUTT_CLEAR) != 0)
        {
          break;
        }

        if (strcmp(buf, searchbuf) == 0)
        {
          if (rd.search_compiled)
          {
            /* do an implicit search-next */
            if (ch == OP_SEARCH)
              ch = OP_SEARCH_NEXT;
            else
              ch = OP_SEARCH_OPPOSITE;

            wrapped = false;
            goto search_next;
          }
        }

        if (!buf[0])
          break;

        mutt_str_strfcpy(searchbuf, buf, sizeof(searchbuf));

        /* leave search_back alone if ch == OP_SEARCH_NEXT */
        if (ch == OP_SEARCH)
          rd.search_back = false;
        else if (ch == OP_SEARCH_REVERSE)
          rd.search_back = true;

        if (rd.search_compiled)
        {
          regfree(&rd.search_re);
          for (size_t i = 0; i < rd.last_line; i++)
          {
            if (rd.line_info[i].search)
              FREE(&(rd.line_info[i].search));
            rd.line_info[i].search_cnt = -1;
          }
        }

        int rflags = mutt_mb_is_lower(searchbuf) ? REG_ICASE : 0;
        int err = REGCOMP(&rd.search_re, searchbuf, REG_NEWLINE | rflags);
        if (err != 0)
        {
          regerror(err, &rd.search_re, buf, sizeof(buf));
          mutt_error("%s", buf);
          for (size_t i = 0; i < rd.max_line; i++)
          {
            /* cleanup */
            if (rd.line_info[i].search)
              FREE(&(rd.line_info[i].search));
            rd.line_info[i].search_cnt = -1;
          }
          rd.search_flag = 0;
          rd.search_compiled = false;
        }
        else
        {
          rd.search_compiled = true;
          /* update the search pointers */
          int line_num = 0;
          while (display_line(rd.fp, &rd.last_pos, &rd.line_info, line_num,
                              &rd.last_line, &rd.max_line,
                              MUTT_SEARCH | (flags & MUTT_PAGER_NSKIP) | (flags & MUTT_PAGER_NOWRAP),
                              &rd.quote_list, &rd.q_level, &rd.force_redraw,
                              &rd.search_re, rd.pager_window) == 0)
          {
            line_num++;
          }

          if (!rd.search_back)
          {
            /* searching forward */
            int i;
            for (i = rd.topline; i < rd.last_line; i++)
            {
              if ((!rd.hide_quoted || (rd.line_info[i].type != MT_COLOR_QUOTED)) &&
                  !rd.line_info[i].continuation && (rd.line_info[i].search_cnt > 0))
              {
                break;
              }
            }

            if (i < rd.last_line)
              rd.topline = i;
          }
          else
          {
            /* searching backward */
            int i;
            for (i = rd.topline; i >= 0; i--)
            {
              if ((!rd.hide_quoted || (rd.line_info[i].type != MT_COLOR_QUOTED)) &&
                  !rd.line_info[i].continuation && (rd.line_info[i].search_cnt > 0))
              {
                break;
              }
            }

            if (i >= 0)
              rd.topline = i;
          }

          if (rd.line_info[rd.topline].search_cnt == 0)
          {
            rd.search_flag = 0;
            mutt_error(_("Not found"));
          }
          else
          {
            rd.search_flag = MUTT_SEARCH;
            /* give some context for search results */
            if (C_SearchContext < rd.pager_window->rows)
              searchctx = C_SearchContext;
            else
              searchctx = 0;
            if (rd.topline - searchctx > 0)
              rd.topline -= searchctx;
          }
        }
        pager_menu->redraw = REDRAW_BODY;
        break;

      case OP_SEARCH_TOGGLE:
        if (rd.search_compiled)
        {
          rd.search_flag ^= MUTT_SEARCH;
          pager_menu->redraw = REDRAW_BODY;
        }
        break;

      case OP_SORT:
      case OP_SORT_REVERSE:
        CHECK_MODE(IsEmail(extra))
        if (mutt_select_sort((ch == OP_SORT_REVERSE)) == 0)
        {
          OptNeedResort = true;
          ch = -1;
          rc = OP_DISPLAY_MESSAGE;
        }
        break;

      case OP_HELP:
        /* don't let the user enter the help-menu from the help screen! */
        if (!InHelp)
        {
          InHelp = 1;
          mutt_help(MENU_PAGER);
          pager_menu->redraw = REDRAW_FULL;
          InHelp = 0;
        }
        else
          mutt_error(_("Help is currently being shown"));
        break;

      case OP_PAGER_HIDE_QUOTED:
        if (rd.has_types)
        {
          rd.hide_quoted ^= MUTT_HIDE;
          if (rd.hide_quoted && (rd.line_info[rd.topline].type == MT_COLOR_QUOTED))
            rd.topline = up_n_lines(1, rd.line_info, rd.topline, rd.hide_quoted);
          else
            pager_menu->redraw = REDRAW_BODY;
        }
        break;

      case OP_PAGER_SKIP_QUOTED:
        if (rd.has_types)
        {
          int dretval = 0;
          int new_topline = rd.topline;

          /* Skip all the email headers */
          if (ISHEADER(rd.line_info[new_topline].type))
          {
            while ((new_topline < rd.last_line ||
                    (0 == (dretval = display_line(
                               rd.fp, &rd.last_pos, &rd.line_info, new_topline, &rd.last_line,
                               &rd.max_line, MUTT_TYPES | (flags & MUTT_PAGER_NOWRAP),
                               &rd.quote_list, &rd.q_level, &rd.force_redraw,
                               &rd.search_re, rd.pager_window)))) &&
                   ISHEADER(rd.line_info[new_topline].type))
            {
              new_topline++;
            }
            rd.topline = new_topline;
            break;
          }

          while (((new_topline + C_SkipQuotedOffset) < rd.last_line ||
                  (0 == (dretval = display_line(
                             rd.fp, &rd.last_pos, &rd.line_info, new_topline, &rd.last_line,
                             &rd.max_line, MUTT_TYPES | (flags & MUTT_PAGER_NOWRAP),
                             &rd.quote_list, &rd.q_level, &rd.force_redraw,
                             &rd.search_re, rd.pager_window)))) &&
                 rd.line_info[new_topline + C_SkipQuotedOffset].type != MT_COLOR_QUOTED)
          {
            new_topline++;
          }

          if (dretval < 0)
          {
            mutt_error(_("No more quoted text"));
            break;
          }

          while (((new_topline + C_SkipQuotedOffset) < rd.last_line ||
                  (0 == (dretval = display_line(
                             rd.fp, &rd.last_pos, &rd.line_info, new_topline, &rd.last_line,
                             &rd.max_line, MUTT_TYPES | (flags & MUTT_PAGER_NOWRAP),
                             &rd.quote_list, &rd.q_level, &rd.force_redraw,
                             &rd.search_re, rd.pager_window)))) &&
                 rd.line_info[new_topline + C_SkipQuotedOffset].type == MT_COLOR_QUOTED)
          {
            new_topline++;
          }

          if (dretval < 0)
          {
            mutt_error(_("No more unquoted text after quoted text"));
            break;
          }
          rd.topline = new_topline;
        }
        break;

      case OP_PAGER_BOTTOM: /* move to the end of the file */
        if (rd.line_info[rd.curline].offset < (rd.sb.st_size - 1))
        {
          int line_num = rd.curline;
          /* make sure the types are defined to the end of file */
          while (display_line(rd.fp, &rd.last_pos, &rd.line_info, line_num, &rd.last_line,
                              &rd.max_line, rd.has_types | (flags & MUTT_PAGER_NOWRAP),
                              &rd.quote_list, &rd.q_level, &rd.force_redraw,
                              &rd.search_re, rd.pager_window) == 0)
          {
            line_num++;
          }
          rd.topline = up_n_lines(rd.pager_window->rows, rd.line_info,
                                  rd.last_line, rd.hide_quoted);
        }
        else
          mutt_error(_("Bottom of message is shown"));
        break;

      case OP_REDRAW:
        clearok(stdscr, true);
        pager_menu->redraw = REDRAW_FULL;
        break;

      case OP_NULL:
        km_error_key(MENU_PAGER);
        break;

        /* --------------------------------------------------------------------
         * The following are operations on the current message rather than
         * adjusting the view of the message.  */

      case OP_BOUNCE_MESSAGE:
      {
        struct Mailbox *m = Context ? Context->mailbox : NULL;
        CHECK_MODE(IsEmail(extra) || IsMsgAttach(extra))
        CHECK_ATTACH;
        if (IsMsgAttach(extra))
          mutt_attach_bounce(m, extra->fp, extra->actx, extra->body);
        else
        {
          struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
          el_add_email(&el, extra->email);
          ci_bounce_message(m, &el);
          el_free(&el);
        }
        break;
      }

      case OP_RESEND:
        CHECK_MODE(IsEmail(extra) || IsMsgAttach(extra))
        CHECK_ATTACH;
        if (IsMsgAttach(extra))
          mutt_attach_resend(extra->fp, extra->actx, extra->body);
        else
          mutt_resend_message(NULL, extra->ctx, extra->email);
        pager_menu->redraw = REDRAW_FULL;
        break;

      case OP_COMPOSE_TO_SENDER:
        CHECK_MODE(IsEmail(extra) || IsMsgAttach(extra));
        CHECK_ATTACH;
        if (IsMsgAttach(extra))
          mutt_attach_mail_sender(extra->fp, extra->email, extra->actx, extra->body);
        else
        {
          struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
          el_add_email(&el, extra->email);
          ci_send_message(SEND_TO_SENDER, NULL, NULL, extra->ctx, &el);
          el_free(&el);
        }
        pager_menu->redraw = REDRAW_FULL;
        break;

      case OP_CHECK_TRADITIONAL:
        CHECK_MODE(IsEmail(extra));
        if (!(WithCrypto & APPLICATION_PGP))
          break;
        if (!(extra->email->security & PGP_TRADITIONAL_CHECKED))
        {
          ch = -1;
          rc = OP_CHECK_TRADITIONAL;
        }
        break;

      case OP_CREATE_ALIAS:
        CHECK_MODE(IsEmail(extra) || IsMsgAttach(extra));
        if (IsMsgAttach(extra))
          mutt_alias_create(extra->body->email->env, NULL);
        else
          mutt_alias_create(extra->email->env, NULL);
        break;

      case OP_PURGE_MESSAGE:
      case OP_DELETE:
        CHECK_MODE(IsEmail(extra));
        CHECK_READONLY;
        /* L10N: CHECK_ACL */
        CHECK_ACL(MUTT_ACL_DELETE, _("Cannot delete message"));

        mutt_set_flag(Context->mailbox, extra->email, MUTT_DELETE, true);
        mutt_set_flag(Context->mailbox, extra->email, MUTT_PURGE, (ch == OP_PURGE_MESSAGE));
        if (C_DeleteUntag)
          mutt_set_flag(Context->mailbox, extra->email, MUTT_TAG, false);
        pager_menu->redraw |= REDRAW_STATUS | REDRAW_INDEX;
        if (C_Resolve)
        {
          ch = -1;
          rc = OP_MAIN_NEXT_UNDELETED;
        }
        break;

      case OP_MAIN_SET_FLAG:
      case OP_MAIN_CLEAR_FLAG:
      {
        CHECK_MODE(IsEmail(extra));
        CHECK_READONLY;

        struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
        el_add_email(&el, extra->email);

        if (mutt_change_flag(Context->mailbox, &el, (ch == OP_MAIN_SET_FLAG)) == 0)
          pager_menu->redraw |= REDRAW_STATUS | REDRAW_INDEX;
        if (extra->email->deleted && C_Resolve)
        {
          ch = -1;
          rc = OP_MAIN_NEXT_UNDELETED;
        }
        el_free(&el);
        break;
      }

      case OP_DELETE_THREAD:
      case OP_DELETE_SUBTHREAD:
      case OP_PURGE_THREAD:
      {
        CHECK_MODE(IsEmail(extra));
        CHECK_READONLY;
        /* L10N: CHECK_ACL */
        /* L10N: Due to the implementation details we do not know whether we
           delete zero, 1, 12, ... messages. So in English we use
           "messages". Your language might have other means to express this.
         */
        CHECK_ACL(MUTT_ACL_DELETE, _("Cannot delete messages"));

        int subthread = (ch == OP_DELETE_SUBTHREAD);
        int r = mutt_thread_set_flag(extra->email, MUTT_DELETE, 1, subthread);
        if (r == -1)
          break;
        if (ch == OP_PURGE_THREAD)
        {
          r = mutt_thread_set_flag(extra->email, MUTT_PURGE, true, subthread);
          if (r == -1)
            break;
        }

        if (C_DeleteUntag)
          mutt_thread_set_flag(extra->email, MUTT_TAG, 0, subthread);
        if (C_Resolve)
        {
          rc = OP_MAIN_NEXT_UNDELETED;
          ch = -1;
        }

        if (!C_Resolve && C_PagerIndexLines)
          pager_menu->redraw = REDRAW_FULL;
        else
          pager_menu->redraw |= REDRAW_STATUS | REDRAW_INDEX;

        break;
      }

      case OP_DISPLAY_ADDRESS:
        CHECK_MODE(IsEmail(extra) || IsMsgAttach(extra));
        if (IsMsgAttach(extra))
          mutt_display_address(extra->body->email->env);
        else
          mutt_display_address(extra->email->env);
        break;

      case OP_ENTER_COMMAND:
        old_PagerIndexLines = C_PagerIndexLines;

        mutt_enter_command();
        pager_menu->redraw = REDRAW_FULL;

        if (OptNeedResort)
        {
          OptNeedResort = false;
          CHECK_MODE(IsEmail(extra));
          OptNeedResort = true;
        }

        if (old_PagerIndexLines != C_PagerIndexLines)
        {
          if (rd.index)
            mutt_menu_destroy(&rd.index);
          rd.index = NULL;
        }

        if ((pager_menu->redraw & REDRAW_FLOW) && (flags & MUTT_PAGER_RETWINCH))
        {
          ch = -1;
          rc = OP_REFORMAT_WINCH;
          continue;
        }

        ch = 0;
        break;

      case OP_FLAG_MESSAGE:
        CHECK_MODE(IsEmail(extra));
        CHECK_READONLY;
        /* L10N: CHECK_ACL */
        CHECK_ACL(MUTT_ACL_WRITE, "Cannot flag message");

        mutt_set_flag(Context->mailbox, extra->email, MUTT_FLAG, !extra->email->flagged);
        pager_menu->redraw |= REDRAW_STATUS | REDRAW_INDEX;
        if (C_Resolve)
        {
          ch = -1;
          rc = OP_MAIN_NEXT_UNDELETED;
        }
        break;

      case OP_PIPE:
        CHECK_MODE(IsEmail(extra) || IsAttach(extra));
        if (IsAttach(extra))
          mutt_pipe_attachment_list(extra->actx, extra->fp, false, extra->body, false);
        else
        {
          struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
          el_add_tagged(&el, extra->ctx, extra->email, false);
          mutt_pipe_message(extra->ctx->mailbox, &el);
          el_free(&el);
        }
        break;

      case OP_PRINT:
        CHECK_MODE(IsEmail(extra) || IsAttach(extra));
        if (IsAttach(extra))
          mutt_print_attachment_list(extra->actx, extra->fp, false, extra->body);
        else
        {
          struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
          el_add_tagged(&el, extra->ctx, extra->email, false);
          mutt_print_message(extra->ctx->mailbox, &el);
          el_free(&el);
        }
        break;

      case OP_MAIL:
        CHECK_MODE(IsEmail(extra) && !IsAttach(extra));
        CHECK_ATTACH;
        ci_send_message(0, NULL, NULL, extra->ctx, NULL);
        pager_menu->redraw = REDRAW_FULL;
        break;

#ifdef USE_NNTP
      case OP_POST:
        CHECK_MODE(IsEmail(extra) && !IsAttach(extra));
        CHECK_ATTACH;
        if (extra->ctx && (extra->ctx->mailbox->magic == MUTT_NNTP) &&
            !((struct NntpMboxData *) extra->ctx->mailbox->mdata)->allowed && (query_quadoption(C_PostModerated, _("Posting to this group not allowed, may be moderated. Continue?")) != MUTT_YES))
        {
          break;
        }
        ci_send_message(SEND_NEWS, NULL, NULL, extra->ctx, NULL);
        pager_menu->redraw = REDRAW_FULL;
        break;

      case OP_FORWARD_TO_GROUP:
        CHECK_MODE(IsEmail(extra) || IsMsgAttach(extra));
        CHECK_ATTACH;
        if (extra->ctx && (extra->ctx->mailbox->magic == MUTT_NNTP) &&
            !((struct NntpMboxData *) extra->ctx->mailbox->mdata)->allowed && (query_quadoption(C_PostModerated, _("Posting to this group not allowed, may be moderated. Continue?")) != MUTT_YES))
        {
          break;
        }
        if (IsMsgAttach(extra))
          mutt_attach_forward(extra->fp, extra->email, extra->actx, extra->body, SEND_NEWS);
        else
        {
          struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
          el_add_email(&el, extra->email);
          ci_send_message(SEND_NEWS | SEND_FORWARD, NULL, NULL, extra->ctx, &el);
          el_free(&el);
        }
        pager_menu->redraw = REDRAW_FULL;
        break;

      case OP_FOLLOWUP:
        CHECK_MODE(IsEmail(extra) || IsMsgAttach(extra));
        CHECK_ATTACH;

        if (IsMsgAttach(extra))
          followup_to = extra->body->email->env->followup_to;
        else
          followup_to = extra->email->env->followup_to;

        if (!followup_to || (mutt_str_strcasecmp(followup_to, "poster") != 0) ||
            (query_quadoption(C_FollowupToPoster,
                              _("Reply by mail as poster prefers?")) != MUTT_YES))
        {
          if (extra->ctx && (extra->ctx->mailbox->magic == MUTT_NNTP) &&
              !((struct NntpMboxData *) extra->ctx->mailbox->mdata)->allowed && (query_quadoption(C_PostModerated, _("Posting to this group not allowed, may be moderated. Continue?")) != MUTT_YES))
          {
            break;
          }
          if (IsMsgAttach(extra))
          {
            mutt_attach_reply(extra->fp, extra->email, extra->actx, extra->body,
                              SEND_NEWS | SEND_REPLY);
          }
          else
          {
            struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
            el_add_email(&el, extra->email);
            ci_send_message(SEND_NEWS | SEND_REPLY, NULL, NULL, extra->ctx, &el);
            el_free(&el);
          }
          pager_menu->redraw = REDRAW_FULL;
          break;
        }
#endif
      /* fallthrough */
      case OP_REPLY:
      case OP_GROUP_REPLY:
      case OP_GROUP_CHAT_REPLY:
      case OP_LIST_REPLY:
      {
        CHECK_MODE(IsEmail(extra) || IsMsgAttach(extra));
        CHECK_ATTACH;

        SendFlags replyflags =
            SEND_REPLY | (ch == OP_GROUP_REPLY ? SEND_GROUP_REPLY : 0) |
            (ch == OP_GROUP_CHAT_REPLY ? SEND_GROUP_CHAT_REPLY : 0) |
            (ch == OP_LIST_REPLY ? SEND_LIST_REPLY : 0);

        if (IsMsgAttach(extra))
          mutt_attach_reply(extra->fp, extra->email, extra->actx, extra->body, replyflags);
        else
        {
          struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
          el_add_email(&el, extra->email);
          ci_send_message(replyflags, NULL, NULL, extra->ctx, &el);
          el_free(&el);
        }
        pager_menu->redraw = REDRAW_FULL;
        break;
      }

      case OP_RECALL_MESSAGE:
      {
        CHECK_MODE(IsEmail(extra) && !IsAttach(extra));
        CHECK_ATTACH;
        struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
        el_add_email(&el, extra->email);
        ci_send_message(SEND_POSTPONED, NULL, NULL, extra->ctx, &el);
        el_free(&el);
        pager_menu->redraw = REDRAW_FULL;
        break;
      }

      case OP_FORWARD_MESSAGE:
        CHECK_MODE(IsEmail(extra) || IsMsgAttach(extra));
        CHECK_ATTACH;
        if (IsMsgAttach(extra))
          mutt_attach_forward(extra->fp, extra->email, extra->actx, extra->body, 0);
        else
        {
          struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
          el_add_email(&el, extra->email);
          ci_send_message(SEND_FORWARD, NULL, NULL, extra->ctx, &el);
          el_free(&el);
        }
        pager_menu->redraw = REDRAW_FULL;
        break;

      case OP_DECRYPT_SAVE:
        if (!WithCrypto)
        {
          ch = -1;
          break;
        }
      /* fallthrough */
      case OP_SAVE:
        if (IsAttach(extra))
        {
          mutt_save_attachment_list(extra->actx, extra->fp, false, extra->body,
                                    extra->email, NULL);
          break;
        }
      /* fallthrough */
      case OP_COPY_MESSAGE:
      case OP_DECODE_SAVE:
      case OP_DECODE_COPY:
      case OP_DECRYPT_COPY:
      {
        if (!(WithCrypto != 0) && (ch == OP_DECRYPT_COPY))
        {
          ch = -1;
          break;
        }
        CHECK_MODE(IsEmail(extra));
        struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
        el_add_email(&el, extra->email);
        if ((mutt_save_message(Context->mailbox, &el,
                               (ch == OP_DECRYPT_SAVE) || (ch == OP_SAVE) || (ch == OP_DECODE_SAVE),
                               (ch == OP_DECODE_SAVE) || (ch == OP_DECODE_COPY),
                               (ch == OP_DECRYPT_SAVE) || (ch == OP_DECRYPT_COPY)) == 0) &&
            ((ch == OP_SAVE) || (ch == OP_DECODE_SAVE) || (ch == OP_DECRYPT_SAVE)))
        {
          if (C_Resolve)
          {
            ch = -1;
            rc = OP_MAIN_NEXT_UNDELETED;
          }
          else
            pager_menu->redraw |= REDRAW_STATUS | REDRAW_INDEX;
        }
        el_free(&el);
        break;
      }

      case OP_SHELL_ESCAPE:
        mutt_shell_escape();
        break;

      case OP_TAG:
        CHECK_MODE(IsEmail(extra));
        if (Context)
        {
          mutt_set_flag(Context->mailbox, extra->email, MUTT_TAG, !extra->email->tagged);

          Context->last_tag =
              extra->email->tagged ?
                  extra->email :
                  ((Context->last_tag == extra->email && !extra->email->tagged) ?
                       NULL :
                       Context->last_tag);
        }

        pager_menu->redraw |= REDRAW_STATUS | REDRAW_INDEX;
        if (C_Resolve)
        {
          ch = -1;
          rc = OP_NEXT_ENTRY;
        }
        break;

      case OP_TOGGLE_NEW:
        CHECK_MODE(IsEmail(extra));
        CHECK_READONLY;
        /* L10N: CHECK_ACL */
        CHECK_ACL(MUTT_ACL_SEEN, _("Cannot toggle new"));

        if (extra->email->read || extra->email->old)
          mutt_set_flag(Context->mailbox, extra->email, MUTT_NEW, true);
        else if (!first)
          mutt_set_flag(Context->mailbox, extra->email, MUTT_READ, true);
        first = false;
        Context->msgnotreadyet = -1;
        pager_menu->redraw |= REDRAW_STATUS | REDRAW_INDEX;
        if (C_Resolve)
        {
          ch = -1;
          rc = OP_MAIN_NEXT_UNDELETED;
        }
        break;

      case OP_UNDELETE:
        CHECK_MODE(IsEmail(extra));
        CHECK_READONLY;
        /* L10N: CHECK_ACL */
        CHECK_ACL(MUTT_ACL_DELETE, _("Cannot undelete message"));

        mutt_set_flag(Context->mailbox, extra->email, MUTT_DELETE, false);
        mutt_set_flag(Context->mailbox, extra->email, MUTT_PURGE, false);
        pager_menu->redraw |= REDRAW_STATUS | REDRAW_INDEX;
        if (C_Resolve)
        {
          ch = -1;
          rc = OP_NEXT_ENTRY;
        }
        break;

      case OP_UNDELETE_THREAD:
      case OP_UNDELETE_SUBTHREAD:
      {
        CHECK_MODE(IsEmail(extra));
        CHECK_READONLY;
        /* L10N: CHECK_ACL */
        /* L10N: Due to the implementation details we do not know whether we
           undelete zero, 1, 12, ... messages. So in English we use
           "messages". Your language might have other means to express this. */
        CHECK_ACL(MUTT_ACL_DELETE, _("Cannot undelete messages"));

        int r = mutt_thread_set_flag(extra->email, MUTT_DELETE, false,
                                     (ch != OP_UNDELETE_THREAD));
        if (r != -1)
        {
          r = mutt_thread_set_flag(extra->email, MUTT_PURGE, false,
                                   (ch != OP_UNDELETE_THREAD));
        }
        if (r != -1)
        {
          if (C_Resolve)
          {
            rc = (ch == OP_DELETE_THREAD) ? OP_MAIN_NEXT_THREAD : OP_MAIN_NEXT_SUBTHREAD;
            ch = -1;
          }

          if (!C_Resolve && C_PagerIndexLines)
            pager_menu->redraw = REDRAW_FULL;
          else
            pager_menu->redraw |= REDRAW_STATUS | REDRAW_INDEX;
        }
        break;
      }

      case OP_VERSION:
        mutt_message(mutt_make_version());
        break;

      case OP_MAILBOX_LIST:
        mutt_mailbox_list();
        break;

      case OP_VIEW_ATTACHMENTS:
        if (flags & MUTT_PAGER_ATTACHMENT)
        {
          ch = -1;
          rc = OP_ATTACH_COLLAPSE;
          break;
        }
        CHECK_MODE(IsEmail(extra));
        mutt_view_attachments(extra->email);
        if (Context && extra->email->attach_del)
          Context->mailbox->changed = true;
        pager_menu->redraw = REDRAW_FULL;
        break;

      case OP_MAIL_KEY:
      {
        if (!(WithCrypto & APPLICATION_PGP))
        {
          ch = -1;
          break;
        }
        CHECK_MODE(IsEmail(extra));
        CHECK_ATTACH;
        struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
        el_add_email(&el, extra->email);
        ci_send_message(SEND_KEY, NULL, NULL, extra->ctx, &el);
        el_free(&el);
        pager_menu->redraw = REDRAW_FULL;
        break;
      }

      case OP_EDIT_LABEL:
      {
        CHECK_MODE(IsEmail(extra));

        struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
        el_add_email(&el, extra->email);
        rc = mutt_label_message(Context->mailbox, &el);
        el_free(&el);

        if (rc > 0)
        {
          Context->mailbox->changed = true;
          pager_menu->redraw = REDRAW_FULL;
          mutt_message(ngettext("%d label changed", "%d labels changed", rc), rc);
        }
        else
        {
          mutt_message(_("No labels changed"));
        }
        break;
      }

      case OP_FORGET_PASSPHRASE:
        crypt_forget_passphrase();
        break;

      case OP_EXTRACT_KEYS:
      {
        if (!WithCrypto)
        {
          ch = -1;
          break;
        }
        CHECK_MODE(IsEmail(extra));
        struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
        el_add_email(&el, extra->email);
        crypt_extract_keys_from_messages(&el);
        el_free(&el);
        pager_menu->redraw = REDRAW_FULL;
        break;
      }

      case OP_WHAT_KEY:
        mutt_what_key();
        break;

      case OP_CHECK_STATS:
        mutt_check_stats();
        break;

#ifdef USE_SIDEBAR
      case OP_SIDEBAR_NEXT:
      case OP_SIDEBAR_NEXT_NEW:
      case OP_SIDEBAR_PAGE_DOWN:
      case OP_SIDEBAR_PAGE_UP:
      case OP_SIDEBAR_PREV:
      case OP_SIDEBAR_PREV_NEW:
        mutt_sb_change_mailbox(ch);
        break;

      case OP_SIDEBAR_TOGGLE_VISIBLE:
        bool_str_toggle(Config, "sidebar_visible", NULL);
        mutt_window_reflow();
        break;
#endif

      default:
        ch = -1;
        break;
    }
  }

  mutt_file_fclose(&rd.fp);
  if (IsEmail(extra))
  {
    if (Context)
      Context->msgnotreadyet = -1;
    switch (rc)
    {
      case -1:
      case OP_DISPLAY_HEADERS:
        mutt_clear_pager_position();
        break;
      default:
        TopLine = rd.topline;
        OldHdr = extra->email;
        break;
    }
  }

  cleanup_quote(&rd.quote_list);

  for (size_t i = 0; i < rd.max_line; i++)
  {
    FREE(&(rd.line_info[i].syntax));
    if (rd.search_compiled && rd.line_info[i].search)
      FREE(&(rd.line_info[i].search));
  }
  if (rd.search_compiled)
  {
    regfree(&rd.search_re);
    rd.search_compiled = false;
  }
  FREE(&rd.line_info);
  mutt_menu_pop_current(pager_menu);
  mutt_menu_destroy(&pager_menu);
  if (rd.index)
    mutt_menu_destroy(&rd.index);

  FREE(&rd.index_status_window);
  FREE(&rd.index_window);
  FREE(&rd.pager_status_window);
  FREE(&rd.pager_window);

  return (rc != -1) ? rc : 0;
}

/*****************************************************************************\
 *  $Id: genders.c,v 1.99 2004-04-22 22:44:36 achu Exp $
 *****************************************************************************
 *  Copyright (C) 2001-2003 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Jim Garlick <garlick@llnl.gov> and Albert Chu <chu11@llnl.gov>.
 *  UCRL-CODE-2003-004.
 *
 *  This file is part of Genders, a cluster configuration database.
 *  For details, see <http://www.llnl.gov/linux/genders/>.
 *
 *  Genders is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  Genders is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Genders; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "genders.h"
#include "fd.h"
#include "list.h"
#include "hash.h"
#include "hostlist.h"

#define GENDERS_MAGIC_NUM         0xdeadbeef

#define GENDERS_READLINE_BUFLEN   65536

#define GENDERS_HASH_MULTIPLIER   2

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN            64
#endif

#ifndef MAX
#define MAX(x,y)                  ((x > y) ? x : y)
#endif

/* stores node name and a list of pointers to attrval lists containing
 * the attributes and values of this node.  The pointers point to
 * lists stored within the attrvalslist parameter of the genders
 * handle.
 */
struct genders_node {
  char name[MAXHOSTNAMELEN+1];
  List attrlist;
  int attrcount;
};
typedef struct genders_node *genders_node_t;

/* stores attribute name and its value (if no value, val == NULL) */
struct genders_attrval {
  char *attr;
  char *val;
};
typedef struct genders_attrval *genders_attrval_t;

/* Genders handle, caches all information loaded from a genders
 * database.  Consider the following genders database
 *
 * nodename[1-2]  attrname1=val1,attrname2=val2
 * nodename1      attrname3=val3,attrname4
 * nodename2      attrname5   
 * nodename3      attrname6
 *
 * After the genders database has been loaded using genders_load_data,
 * the lists and data in the handle can be viewed like the following:
 *
 * magic = GENDERS_MAGIC_NUM
 * errnum = current error code
 * is_loaded = 1
 * numnodes = 3
 * numattrs = 6 
 * maxattrs = 4
 * maxnodelen = 9
 * maxattrlen = 5
 * maxvallen = 4
 * nodename = localhost
 * nodeslist = node1 -> node2 -> node3 -> \0
 *    node1.name = nodename1, node1.attrlist = listptr1 -> listptr2 -> \0
 *    node2.name = nodename2, node2.attrlist = listptr1 -> listptr3 -> \0
 *    node3.name = nodename3, node3.attrlist = listptr4 -> \0
 * attrvalslist = listptr1 -> listptr2 -> listptr3 -> listptr4 -> \0
 *    listptr1 = attr1 -> attr2 -> \0
 *    listptr2 = attr3 -> attr4 -> \0
 *    listptr3 = attr5 -> \0
 *    listptr4 = attr6 -> \0
 *      attr1.attr = attrname1, attr1.val = val1
 *      attr2.attr = attrname2, attr2.val = val2
 *      attr3.attr = attrname3, attr3.val = val3
 *      attr4.attr = attrname4, attr4.val = NULL
 *      attr5.attr = attrname5, attr5.val = NULL
 *      attr6.attr = attrname6, attr6.val = NULL
 * attrslist = attrname1 -> attrname2 -> attrname3 -> attrname4 -> 
 *             attrname5 -> attrname6 -> \0
 * node_index = hash table with
 *              KEY(nodename1) -> node1
 *              KEY(nodename2) -> node2
 *              KEY(nodename3) -> node3
 * valbuf -> buffer of length 5 (maxvallen + 1) 
 */
struct genders {
  int magic;                        /* magic number */ 
  int errnum;                       /* error code */
  int is_loaded;                    /* genders data loaded? */
  int numnodes;                     /* number of nodes in database */
  int numattrs;                     /* number of attrs in database */
  int maxattrs;                     /* max attrs any one node has */
  int maxnodelen;                   /* max node name length */
  int maxattrlen;                   /* max attr name length */
  int maxvallen;                    /* max value name length */
  char nodename[MAXHOSTNAMELEN+1];  /* local hostname */
  List nodeslist;                   /* Lists of genders_node */
  List attrvalslist;                /* Lists of ptrs to Lists of genders_attrvals */
  List attrslist;                   /* List of unique attribute strings */
  hash_t node_index;                /* Index table for quicker node access */
  char *valbuf;                     /* Buffer for value substitution */
  
};

/* Error messages */
static char * errmsg[] = {
  "success",
  "genders handle is null",
  "error opening genders file",
  "error reading genders file",
  "genders file parse error",
  "genders data not loaded",
  "genders data already loaded",
  "array or string passed in not large enough to store result",
  "incorrect parameters passed in",
  "null pointer reached in list", 
  "node not found",
  "out of memory",
  "genders handle magic number incorrect, improper handle passed in",
  "unknown internal error"
  "error number out of range",
};

/* List API Helper Functions */

static int 
_is_all(void *x, void *key) 
{
  return 1;
}

static int 
_is_str(void *x, void *key) 
{
  if (!strcmp((char *)x, (char *)key))
    return 1;
  return 0;
}

static int 
_is_node(void *x, void *key) 
{
  genders_node_t n = (genders_node_t)x;
  if (!strcmp(n->name, (char *)key))
    return 1;
  return 0;
}

static int
_is_attr_in_attrvals(void *x, void *key)
{
  genders_attrval_t av = (genders_attrval_t)x;
  if (!strcmp(av->attr, (char *)key))
    return 1;
  return 0;
}

static int 
_is_attr_in_attrlist(void *x, void *key) 
{
  List attrvals = (List)x;
  if (list_find_first(attrvals, _is_attr_in_attrvals, key))
    return 1;
  return 0;
}

static void 
_free_genders_node(void *x) 
{
  genders_node_t n = (genders_node_t)x;
  list_destroy(n->attrlist);
  free(n);
}

static void 
_free_genders_attrval(void *x) 
{
  genders_attrval_t av = (genders_attrval_t)x;
  free(av->attr);
  free(av->val);
  free(av);
}

static void
_free_attrvallist(void *x)
{
  List attrvals = (List)x;
  list_destroy(attrvals);
}

/* API and API helper functions */

static int 
_handle_error_check(genders_t handle) 
{
  if (!handle || handle->magic != GENDERS_MAGIC_NUM)
    return -1;
  return 0;
}

static int 
_unloaded_handle_error_check(genders_t handle) 
{
  if (_handle_error_check(handle) < 0)
    return -1;

  if (handle->is_loaded) {
    handle->errnum = GENDERS_ERR_ISLOADED;
    return -1;
  }
  
  return 0;
}

static int 
_loaded_handle_error_check(genders_t handle) 
{
  if (_handle_error_check(handle) < 0)
    return -1;

  if (!handle->is_loaded) {
    handle->errnum = GENDERS_ERR_NOTLOADED;
    return -1;
  }
  
  return 0;
}

static int
_find_attrval_in_attrlist(genders_t handle, List attrlist, const char *attr, 
                          genders_attrval_t *avptr)
{
  ListIterator itr = NULL;
  List attrvals;
  int retval = -1;
  
  /* Cannot use _is_attr_in_attrlist, because we need to return
   * the avptr to the user
   */

  *avptr = NULL;

  if (!(itr = list_iterator_create(attrlist))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  while ((attrvals = list_next(itr))) {
    genders_attrval_t av;
    
    if ((av = list_find_first(attrvals, _is_attr_in_attrvals, (char *)attr))) {
      *avptr = av;
      break;
    }
  }

  retval = 0;
 cleanup:
  if (itr)
    list_iterator_destroy(itr);
  return retval;  
}

/* _get_val
 * - Handles substitutions for %n 
 * - Caller responsible that av->val != NULL
 * Returns 1 is substitution occurred, 0 if not 
 */
static int
_get_val(genders_t handle, genders_node_t n, genders_attrval_t av)
{
  char *val, *node, *valbufptr = handle->valbuf;

  if (!strstr(av->val, "%n") && !strstr(av->val, "%%"))
    return 0;

  val = av->val;
  memset(valbufptr, '\0', handle->maxvallen + 1);
  while (*val != '\0') {
    if (*val == '%') {
      if ((*(val + 1)) == '%') {
	*(valbufptr)++ = '%';
	val++;
      }
      else if ((*(val + 1)) == 'n') {
        if ((strlen(av->val) - 2 + strlen(n->name)) > 
	    (handle->maxvallen + 1)) {
          handle->errnum = GENDERS_ERR_INTERNAL;
          return -1;
        }

	node = n->name;
	while (*node != '\0')
	  *(valbufptr)++ = *node++;
	val++;
      }
      else
	*(valbufptr)++ = *val;
    }
    else 
      *(valbufptr)++ = *val;

    val++;
  }

  return 1;
}

static void 
_initialize_handle_info(genders_t handle)
{
  handle->magic = GENDERS_MAGIC_NUM;
  handle->is_loaded = 0;
  handle->numnodes = 0;
  handle->numattrs = 0;
  handle->maxattrs = 0;
  handle->maxnodelen = 0;
  handle->maxattrlen = 0;
  handle->maxvallen = 0;
  memset(handle->nodename,'\0',MAXHOSTNAMELEN+1);
}

genders_t 
genders_handle_create(void) 
{
  genders_t handle = NULL;

  if (!(handle = (genders_t)malloc(sizeof(struct genders))))
    goto cleanup;
  
  _initialize_handle_info(handle);
  handle->nodeslist = NULL;
  handle->attrvalslist = NULL;
  handle->attrslist = NULL;
  handle->node_index = NULL;
  handle->valbuf = NULL;

  if (!(handle->nodeslist = list_create(_free_genders_node)))
    goto cleanup;

  if (!(handle->attrvalslist = list_create(_free_attrvallist)))
    goto cleanup;

  if (!(handle->attrslist = list_create(free)))
    goto cleanup;

  /* Hash node_index created at genders_load_data because we don't
   * know the hash size yet
   */

  /* Valbuf created in genders_load_data after maxvallen is
   * calculated 
   */

  handle->errnum = GENDERS_ERR_SUCCESS;
  return handle;

cleanup:
  if (handle) {
    if (handle->nodeslist)
      list_destroy(handle->nodeslist);
    if (handle->attrvalslist)
      list_destroy(handle->attrvalslist);
    if (handle->attrslist)
      list_destroy(handle->attrslist);
    free(handle);
  }
  return NULL;
}

int 
genders_handle_destroy(genders_t handle)
{
  if (_handle_error_check(handle) < 0)
    return -1;

  list_destroy(handle->nodeslist);
  list_destroy(handle->attrvalslist);
  list_destroy(handle->attrslist);
  if (handle->node_index)
    hash_destroy(handle->node_index);
  free(handle->valbuf);

  /* "clean" handle */
  _initialize_handle_info(handle);
  handle->magic = ~GENDERS_MAGIC_NUM;
  handle->is_loaded = 0;
  handle->nodeslist = NULL;
  handle->attrvalslist = NULL;
  handle->attrslist = NULL;
  handle->node_index = NULL;
  free(handle);
  return 0;
}

static int 
_readline(genders_t handle, int fd, char *buf, int buflen) 
{
  int ret; 

  if ((ret = fd_read_line(fd, buf, buflen)) < 0) {
    handle->errnum = GENDERS_ERR_READ;
    return -1;
  }
  
  /* buflen - 1 b/c fd_read_line guarantees null termination */
  if (ret >= (buflen-1)) {
    handle->errnum = GENDERS_ERR_PARSE;
    return -1;
  }

  return ret;
}

static genders_node_t
_insert_node(genders_t handle, List nodelist, char *nodename)
{
  genders_node_t n = NULL;

  n = list_find_first(nodelist, _is_node, nodename);

  /* must create if node not yet in list */ 
  if (!n) {
    if (!(n = (genders_node_t)malloc(sizeof(struct genders_node)))) {
      handle->errnum = GENDERS_ERR_OUTMEM;
      goto cleanup;
    }
    memset(n, '\0', sizeof(struct genders_node));

    strcpy(n->name, nodename);    /* length previously asserted */
    n->attrcount = 0;

    if (!(n->attrlist = list_create(NULL))) {
      handle->errnum = GENDERS_ERR_OUTMEM;
      goto cleanup;
    }

    if (!list_append(nodelist, n)) {
      handle->errnum = GENDERS_ERR_INTERNAL;
      goto cleanup;
    }
  }

  return n;

 cleanup:
  if (n) { 
    if (n->attrlist)
      list_destroy(n->attrlist);
    free(n);
  }
  return NULL;
}

static int 
_insert_attrval(genders_t handle, List attrvals, char *attr, char *val) 
{
  genders_attrval_t av = NULL;

  if (!(av = (genders_attrval_t)malloc(sizeof(struct genders_attrval)))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }
  memset(av, '\0', sizeof(struct genders_attrval));

  if (!(av->attr = strdup(attr))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  if (val) {
    if (!(av->val = strdup(val))) {
      handle->errnum = GENDERS_ERR_OUTMEM;
      goto cleanup;
    }
  }
  else 
    av->val = NULL; 

  if (!list_append(attrvals, av)) {
    handle->errnum = GENDERS_ERR_INTERNAL;
    goto cleanup;
  }

  return 0;
 cleanup:
  if (av) {
    if (av->attr)
      free(av->attr);
    if (av->val)
      free(av->val);
    free(av);
  }
  return -1;
}

static int 
_insert_ptr(genders_t handle, List ptrlist, void *ptr) 
{
  if (!list_append(ptrlist, ptr)) {
    handle->errnum = GENDERS_ERR_INTERNAL;
    return -1;
  }
  return 0;
}

static int 
_insert_attr(genders_t handle, char *attr) 
{
  char *attr_new = NULL;

  if (list_find_first(handle->attrslist, _is_str, attr))
    return 0;

  if (!(attr_new = strdup(attr))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    return -1;
  }

  if (!list_append(handle->attrslist, attr_new)) {
    handle->errnum = GENDERS_ERR_INTERNAL;
    free(attr_new);
    return -1;
  }

  return 1;
}

static int
_duplicate_attr_check(genders_t handle, List attrlist, List attrvals)
{
  ListIterator itr = NULL;
  genders_attrval_t av = NULL;
  int retval = -1;

  if (!(itr = list_iterator_create(attrvals))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  while ((av = list_next(itr))) {
    if (list_find_first(attrlist, _is_attr_in_attrlist, av->attr)) {
      retval = 1;
      goto cleanup;
    }
  }

  retval = 0;
 cleanup:
  if (itr)
    list_iterator_destroy(itr);
  return retval;
}

/* parse a genders file line
 * - If line_num == 0, parse and store genders data
 * - If line_num > 0, debug genders file, do not store genders data, use
 *   debug lists instead of handle lists for debugging.
 * - Returns -1 on error, 1 if there was a parse error, 0 if no errors
 * - No need to cleanup if line_num == 0, will be done in genders_load_data.
 */
static int 
_parse_line(genders_t handle, char *line, int line_num, FILE *stream, 
	    List debugnodeslist, List debugattrvalslist) 
{
  char *linebuf, *temp, *attr, *nodenames, *node = NULL;
  int rv, retval = -1;
  int max_n_subst_vallen = 0;
  int line_maxnodelen = 0;
  List attrvals = NULL;
  List nodelist = NULL;
  List attrvalslist = NULL;
  genders_node_t n;
  hostlist_t hl = NULL;
  hostlist_iterator_t hlitr = NULL;

  /* "remove" comments */
  if ((temp = strchr(line, '#'))) 
    *temp = '\0';

  /* remove trailing white space, including newline */
  temp = line + strlen(line);
  for (--temp; temp >= line; temp--) {
    if (isspace(*temp))
      *temp = '\0';
    else
      break;
  }

  /* empty line */
  if (*line == '\0')
    return 0;

  /* move forward to node name(s) */
  while(isspace(*line))  
    line++;

  /* get node name(s) */
  if (!(nodenames = strsep(&line, " \t\0")))
    /* should be impossible to hit this */
    return 0;

  /* if strsep() sets line == NULL, line has no attributes */
  if (line) {
    /* move forward to attributes */
    while(isspace(*line))  
      line++;

    /* *line == '\0' means line has no attributes */
    if (*line != '\0') {
      if (strchr(line,' ') || strchr(line,'\t')) {
	if (line_num > 0) {
	  fprintf(stream, "Line %d: white space in attribute list\n", line_num);
	  retval = 1;
	}
	handle->errnum = GENDERS_ERR_PARSE;
	goto cleanup;
      }
      
      if (!(attrvals = list_create(_free_genders_attrval))) {
        handle->errnum = GENDERS_ERR_OUTMEM;
        goto cleanup;
      }

      /* parse attributes */
      attr = strtok_r(line,",\0",&linebuf);
      while (attr) {
	char *val = NULL;
	int is_new_attr;
      
	/* parse value out of attribute */
	if ((val = strchr(attr,'=')))
	  *val++ = '\0'; 

	if (list_find_first(attrvals, _is_attr_in_attrvals, attr) != NULL) {
	  if (line_num > 0) {
	    fprintf(stream, "Line %d: duplicate attribute \"%s\" listed\n",
		    line_num, attr);
	  }
	  handle->errnum = GENDERS_ERR_PARSE;
	  goto cleanup;
	}

	if (_insert_attrval(handle, attrvals, attr, val) < 0)
	  goto cleanup;
	
	if (!line_num) { 
	  if ((is_new_attr = _insert_attr(handle, attr)) < 0)
	    goto cleanup;
	  
	  handle->numattrs += is_new_attr;
	  
	  handle->maxattrlen = MAX(strlen(attr), handle->maxattrlen);
	  if (val) {
	    if (strstr(val, "%n"))
	      max_n_subst_vallen = strlen(val);
	    else
	      handle->maxvallen = MAX(strlen(val), handle->maxvallen);
	  }
	}
	
	attr = strtok_r(NULL,",\0",&linebuf);
      }
    }
  }

  if (!line_num) {
    nodelist = handle->nodeslist;
    attrvalslist = handle->attrvalslist;
  }
  else {
    nodelist = debugnodeslist;
    attrvalslist = debugattrvalslist;
  }

  if (!(hl = hostlist_create(nodenames))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  if (!(hlitr = hostlist_iterator_create(hl))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  while ((node = hostlist_next(hlitr))) {
    if (strlen(node) > MAXHOSTNAMELEN) {
      if (line_num > 0) {
	fprintf(stream, "Line %d: hostname too long\n", line_num);
	retval = 1;
      }
      handle->errnum = GENDERS_ERR_PARSE;
      goto cleanup;
    }
    
    if (strchr(node, '.')) {
      if (line_num > 0) {
	fprintf(stream, "Line %d: node not a shortened hostname\n", line_num);
	retval = 1;
      }
      handle->errnum = GENDERS_ERR_PARSE;
      goto cleanup;
    }

    if (!(n =_insert_node(handle, nodelist, node)))
      goto cleanup;

    if (attrvals) {
      if ((rv = _duplicate_attr_check(handle, n->attrlist, attrvals)) < 0)
        goto cleanup;

      if (rv == 1) {
        if (line_num > 0) {
          fprintf(stream, "Line %d: duplicate attributed listed for node \"%s\"\n",
                  line_num, node);
          retval = 1;
        }
        handle->errnum = GENDERS_ERR_PARSE;
        goto cleanup;
      }
    
      if (_insert_ptr(handle, n->attrlist, attrvals) < 0)
        goto cleanup;

      n->attrcount += list_count(attrvals);
    }

    if (!line_num) {
      handle->maxattrs = MAX(n->attrcount, handle->maxattrs);
      handle->maxnodelen = MAX(strlen(node), handle->maxnodelen);
      line_maxnodelen = MAX(strlen(node), line_maxnodelen);
    }
    
    free(node);
  }
  node = NULL;

  /* %n substituation found on this line, update maxvallen */
  if (max_n_subst_vallen)
    handle->maxvallen = MAX(max_n_subst_vallen - 2 + line_maxnodelen,
			    handle->maxvallen);

  /* Append at the very end, so cleanup area cleaner */
  if (attrvals) {
    if (_insert_ptr(handle, attrvalslist, attrvals) < 0)
      goto cleanup;
    attrvals = NULL;
  }

  retval = 0;
 cleanup:
  if (hl) {
    if (hlitr)
      hostlist_iterator_destroy(hlitr);
    hostlist_destroy(hl);
  }
  if (attrvals)
    list_destroy(attrvals);
  free(node);
  return retval;
}

int 
genders_load_data(genders_t handle, const char *filename) 
{
  char *temp;
  int ret, fd = -1;
  char buf[GENDERS_READLINE_BUFLEN];
  genders_node_t n;
  ListIterator itr = NULL;

  if (_unloaded_handle_error_check(handle) < 0)
    goto cleanup;

  if (!filename)
    filename = GENDERS_DEFAULT_FILE;

  if ((fd = open(filename, O_RDONLY)) < 0) {
    handle->errnum = GENDERS_ERR_OPEN;
    goto cleanup;
  }

  /* parse line by line */
  while ((ret = _readline(handle, fd, buf, GENDERS_READLINE_BUFLEN)) > 0) {
    if (_parse_line(handle, buf, 0, NULL, NULL, NULL) < 0)
      goto cleanup;
  }

  if (ret < 0)
    goto cleanup;

  handle->numnodes = list_count(handle->nodeslist);

  /* umm, where are the nodes? */
  if (!handle->numnodes) {
    handle->errnum = GENDERS_ERR_PARSE;
    goto cleanup;
  }

  if (gethostname(handle->nodename, MAXHOSTNAMELEN+1) < 0) {
    handle->errnum = GENDERS_ERR_INTERNAL;
    goto cleanup;
  }
  handle->nodename[MAXHOSTNAMELEN]='\0';

  /* shorten hostname if necessary */
  if ((temp = strchr(handle->nodename,'.')))
    *temp = '\0';
  
  handle->maxnodelen = MAX(strlen(handle->nodename), handle->maxnodelen);

  /* Create hash node_index for quick searches, make hash larger than
   * numnodes to decrease the probability of hash collisions.
   */

  if (!(handle->node_index = hash_create(handle->numnodes * GENDERS_HASH_MULTIPLIER,
					 (hash_key_f)hash_key_string,
					 (hash_cmp_f)strcmp,
					 (hash_del_f)list_destroy))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  if (!(itr = list_iterator_create(handle->nodeslist))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  while ((n = list_next(itr))) {
    List l = NULL;

    /* No need to call list_iterator_destry in this
     * loop, will be handled in cleanup
     */

    if (!(l = hash_find(handle->node_index, n->name))) {
      if (!(l = list_create(NULL))) {
	handle->errnum = GENDERS_ERR_OUTMEM;
	goto cleanup;
      }

      if (!hash_insert(handle->node_index, n->name, l)) {
	if (errno == ENOMEM)
	  handle->errnum = GENDERS_ERR_OUTMEM;
	else
	  handle->errnum = GENDERS_ERR_INTERNAL;
	list_destroy(l);
	goto cleanup;
      }
    }
    
    if (!list_append(l, n)) {
      /* No need to destroy list, hash_destroy in cleanup will handle
       * it
       */
      handle->errnum = GENDERS_ERR_INTERNAL;
      goto cleanup;
    }
  }

  /* Create a buffer for value substitutions */
  if (!(handle->valbuf = malloc(handle->maxvallen + 1))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  close(fd);
  list_iterator_destroy(itr);

  handle->is_loaded++;
  handle->errnum = GENDERS_ERR_SUCCESS;
  return 0;

cleanup:
  close(fd);
  if (itr)
    list_iterator_destroy(itr);
  if (handle->node_index) {
    hash_destroy(handle->node_index);
    handle->node_index = NULL;
  }
  free(handle->valbuf);
  handle->valbuf = NULL;

  /* Can't pass NULL for key, so pass junk, _is_all() will ensure
   * everything is deleted
   */
  list_delete_all(handle->nodeslist, _is_all, ""); 
  list_delete_all(handle->attrvalslist, _is_all, ""); 
  list_delete_all(handle->attrslist, _is_all, ""); 
  _initialize_handle_info(handle);
  return -1;
}

int 
genders_errnum(genders_t handle) 
{
  if (!handle)
    return GENDERS_ERR_NULLHANDLE;
  else if (handle->magic != GENDERS_MAGIC_NUM)
    return GENDERS_ERR_MAGIC;
  else
    return handle->errnum;
}

char *
genders_strerror(int errnum) 
{
  if (errnum >= GENDERS_ERR_SUCCESS && errnum <= GENDERS_ERR_ERRNUMRANGE)
    return errmsg[errnum];
  else
    return errmsg[GENDERS_ERR_ERRNUMRANGE];
}

char *
genders_errormsg(genders_t handle) 
{
  return genders_strerror(genders_errnum(handle));
}

void 
genders_perror(genders_t handle, const char *msg) 
{
  char *errormsg = genders_strerror(genders_errnum(handle));

  if (!msg)
    fprintf(stderr, "%s\n", errormsg);
  else
    fprintf(stderr, "%s: %s\n", msg, errormsg);
}

int 
genders_getnumnodes(genders_t handle) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  handle->errnum = GENDERS_ERR_SUCCESS;
  return handle->numnodes;
}

int 
genders_getnumattrs(genders_t handle) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  handle->errnum = GENDERS_ERR_SUCCESS;
  return handle->numattrs;
}

int 
genders_getmaxattrs(genders_t handle) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  handle->errnum = GENDERS_ERR_SUCCESS;
  return handle->maxattrs;
}

int 
genders_getmaxnodelen(genders_t handle) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  handle->errnum = GENDERS_ERR_SUCCESS;
  return handle->maxnodelen;
}

int 
genders_getmaxattrlen(genders_t handle) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  handle->errnum = GENDERS_ERR_SUCCESS;
  return handle->maxattrlen;
}

int 
genders_getmaxvallen(genders_t handle) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  handle->errnum = GENDERS_ERR_SUCCESS;
  return handle->maxvallen;
}

static int 
_list_create(genders_t handle, char ***list, int len, int buflen) 
{
  if (len > 0) {
    char **temp;
    int i,j;

    if (!list) {
      handle->errnum = GENDERS_ERR_PARAMETERS;
      return -1;
    }

    if (!(temp = (char **)malloc(sizeof(char *) * len))) {
      handle->errnum = GENDERS_ERR_OUTMEM;
      return -1;
    }

    for (i = 0; i < len; i++) {
      if (!(temp[i] = (char *)malloc(buflen))) {
        for (j = 0; j < i; j++)
          free(temp[j]);
        free(temp);

        handle->errnum = GENDERS_ERR_OUTMEM;
        return -1;
      }
      memset(temp[i], '\0', buflen);
    }

    *list = temp;
  }

  handle->errnum = GENDERS_ERR_SUCCESS;
  return len;
}

static int 
_list_clear(genders_t handle, char **list, int len, int buflen) 
{
  int i;

  if (len > 0) {

    if (!list) {
      handle->errnum = GENDERS_ERR_PARAMETERS;
      return -1;
    }
    
    for (i = 0; i < len; i++) {
      if (!list[i]) {
        handle->errnum = GENDERS_ERR_NULLPTR;
        return -1;
      }
      memset(list[i], '\0', buflen);
    }
  }

  handle->errnum = GENDERS_ERR_SUCCESS;
  return 0;
}

static int 
_list_destroy(genders_t handle, char **list, int len) 
{
  if (len > 0) {
    int i;

    if (!list) {
      handle->errnum = GENDERS_ERR_PARAMETERS;
      return -1;
    }

    for (i = 0; i < len; i++)
      free(list[i]);
    free(list);
  }

  handle->errnum = GENDERS_ERR_SUCCESS;
  return 0;
}

int 
genders_nodelist_create(genders_t handle, char ***list) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  return _list_create(handle, list, handle->numnodes, handle->maxnodelen+1);
}

int 
genders_nodelist_clear(genders_t handle, char **list) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  return _list_clear(handle, list, handle->numnodes, handle->maxnodelen+1);
}

int 
genders_nodelist_destroy(genders_t handle, char **list) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  return _list_destroy(handle, list, handle->numnodes);
}

int 
genders_attrlist_create(genders_t handle, char ***list) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  return _list_create(handle, list, handle->numattrs, handle->maxattrlen+1);
}

int 
genders_attrlist_clear(genders_t handle, char **list) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  return _list_clear(handle, list, handle->numattrs, handle->maxattrlen+1);
}

int 
genders_attrlist_destroy(genders_t handle, char **list) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;
  
  return _list_destroy(handle, list, handle->numattrs);
}

int 
genders_vallist_create(genders_t handle, char ***list) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  return _list_create(handle, list, handle->numattrs, handle->maxvallen+1);
}

int 
genders_vallist_clear(genders_t handle, char **list) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  return _list_clear(handle, list, handle->numattrs, handle->maxvallen+1);
}

int 
genders_vallist_destroy(genders_t handle, char **list) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  return _list_destroy(handle, list, handle->numattrs);
}

int 
genders_getnodename(genders_t handle, char *node, int len) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  if (!node || len <= 0) {
    handle->errnum = GENDERS_ERR_PARAMETERS;
    return -1;
  }

  if ((strlen(handle->nodename) + 1) > len) {
    handle->errnum = GENDERS_ERR_OVERFLOW;
    return -1;
  }

  strcpy(node, handle->nodename);
  handle->errnum = GENDERS_ERR_SUCCESS;
  return 0;
}

static int 
_put_in_list(genders_t handle, char *str, char **list, int index, int len) 
{
  if (index >= len) {
    handle->errnum = GENDERS_ERR_OVERFLOW;
    return -1;
  }

  if (!list[index]) {
    handle->errnum = GENDERS_ERR_NULLPTR;
    return -1;
  }
  
  strcpy(list[index], str); 
  return 0;
}

int 
genders_getnodes(genders_t handle, char *nodes[], int len, 
                 const char *attr, const char *val) 
{
  ListIterator itr = NULL;
  genders_node_t n;
  int index = 0;
  int rv, retval = -1;

  if (_loaded_handle_error_check(handle) < 0)
    goto cleanup;

  if ((!nodes && len > 0) || len < 0) {
    handle->errnum = GENDERS_ERR_PARAMETERS;
    goto cleanup;
  }

  if (!(itr = list_iterator_create(handle->nodeslist))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  while ((n = list_next(itr))) {
    genders_attrval_t av;
    int save = 0;

    if (!attr)
      save++;
    else {
      if (_find_attrval_in_attrlist(handle, n->attrlist, attr, &av) < 0)
	goto cleanup;
      if (av) {
	if (!val)
	  save++;
	else if (av->val) {
	  if ((rv = _get_val(handle, n, av)) < 0)
	    goto cleanup;
	  if ((rv > 0 && !strcmp(handle->valbuf, val)) || !strcmp(av->val, val))
	    save++;
	}
	  
      }
    }
  
    if (save && _put_in_list(handle, n->name, nodes, index++, len) < 0)
      goto cleanup;
  }

  retval = index;
  handle->errnum = GENDERS_ERR_SUCCESS;
 cleanup:
  if (itr)
    list_iterator_destroy(itr);
  return retval;
}

int 
genders_getattr(genders_t handle, char *attrs[], char *vals[], 
                int len, const char *node) 
{
  ListIterator itr = NULL;
  ListIterator avitr = NULL;
  List attrvals;
  List l;
  genders_node_t n;
  int index = 0;
  int rv, retval = -1;

  if (_loaded_handle_error_check(handle) < 0)
    goto cleanup;

  if ((!attrs && len > 0) || len < 0) {
    handle->errnum = GENDERS_ERR_PARAMETERS;
    goto cleanup;
  }

  if (!node)
    node = handle->nodename;

  if ((l = hash_find(handle->node_index, node)) == NULL) {
    handle->errnum = GENDERS_ERR_NOTFOUND;
    return -1;
  }

  if (!(n = list_find_first(l, _is_node, (char *)node))) {
    handle->errnum = GENDERS_ERR_NOTFOUND;
    goto cleanup;
  }

  if (!(itr = list_iterator_create(n->attrlist))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  while ((attrvals = list_next(itr))) {
    genders_attrval_t av;

    if (!(avitr = list_iterator_create(attrvals))) {
      handle->errnum = GENDERS_ERR_OUTMEM;
      goto cleanup;
    }

    while ((av = list_next(avitr))) {
      if (_put_in_list(handle, av->attr, attrs, index, len) < 0)
	goto cleanup;
      
      if (vals && av->val) {
	if ((rv = _get_val(handle, n, av)) < 0)
	  goto cleanup;

	if (_put_in_list(handle, (rv) ? handle->valbuf : av->val, 
			 vals, index, len) < 0)
	  goto cleanup;
      }
      index++;
    }
    list_iterator_destroy(avitr);
  }
  avitr = NULL;

  retval = index;
  handle->errnum = GENDERS_ERR_SUCCESS;
 cleanup:
  if (itr)
    list_iterator_destroy(itr);
  if (avitr)
    list_iterator_destroy(avitr);
  return retval;  
}

int 
genders_getattr_all(genders_t handle, char *attrs[], int len) 
{
  ListIterator itr = NULL; 
  char *attr;
  int index = 0;
  int retval = -1;
  
  if (_loaded_handle_error_check(handle) < 0)
    goto cleanup;

  if ((!attrs && len > 0) || len < 0) {
    handle->errnum = GENDERS_ERR_PARAMETERS;
    goto cleanup;
  }

  if (handle->numattrs > len) {
    handle->errnum = GENDERS_ERR_OVERFLOW;
    goto cleanup;
  }

  if (!(itr = list_iterator_create(handle->attrslist))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  while ((attr = list_next(itr))) {
    if (_put_in_list(handle, attr, attrs, index++, len) < 0)
      goto cleanup;
  }

  retval = index;
  handle->errnum = GENDERS_ERR_SUCCESS;
 cleanup:
  if (itr)
    list_iterator_destroy(itr);
  return retval;  
}

int 
genders_testattr(genders_t handle, const char *node, const char *attr, 
                 char *val, int len) 
{
  genders_node_t n;
  genders_attrval_t av;
  int rv, retval = 0;
  List l;

  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  if (!attr || (val && len <= 0)) {
    handle->errnum = GENDERS_ERR_PARAMETERS;
    return -1;
  }

  if (!node)
    node = handle->nodename;

  if ((l = hash_find(handle->node_index, node)) == NULL) {
    handle->errnum = GENDERS_ERR_NOTFOUND;
    return -1;
  }

  if (!(n = list_find_first(l, _is_node, (char *)node))) {
    handle->errnum = GENDERS_ERR_NOTFOUND;
    return -1;
  }

  if (_find_attrval_in_attrlist(handle, n->attrlist, attr, &av) < 0)
    return -1;

  if (av) {
    if (val && av->val) {
      if ((rv = _get_val(handle, n, av)) < 0)
	return -1;
      
      if (((rv > 0 && strlen(handle->valbuf) + 1) > len) 
	  || (strlen(av->val) + 1) > len) {
	handle->errnum = GENDERS_ERR_OVERFLOW;
	return -1;
      }
      strcpy(val, (rv) ? handle->valbuf : av->val);
    }
    retval = 1;
  }
  
  handle->errnum = GENDERS_ERR_SUCCESS;
  return retval;
}

int 
genders_testattrval(genders_t handle, const char *node, 
                    const char *attr, const char *val) 
{
  genders_node_t n;
  genders_attrval_t av;
  int rv, retval = 0;
  List l;

  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  if (!attr) {
    handle->errnum = GENDERS_ERR_PARAMETERS;
    return -1;
  }

  if (!node)
    node = handle->nodename;

  if ((l = hash_find(handle->node_index, node)) == NULL) {
    handle->errnum = GENDERS_ERR_NOTFOUND;
    return -1;
  }

  if (!(n = list_find_first(l, _is_node, (char *)node))) {
    handle->errnum = GENDERS_ERR_NOTFOUND;
    return -1;
  }
  
  if (_find_attrval_in_attrlist(handle, n->attrlist, attr, &av) < 0)
    return -1;
  
  if (av) {
    if (val) {
      if (av->val) {
        if ((rv = _get_val(handle, n, av)) < 0)
          return -1;

        if ((rv > 0 && !strcmp(handle->valbuf, val)) || !strcmp(av->val, val))
          retval = 1;
      }
    }
    else
      retval = 1;
  }
  
  handle->errnum = GENDERS_ERR_SUCCESS;
  return retval;
}

int 
genders_isnode(genders_t handle, const char *node) 
{
  List l;

  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  if (!node)
    node = handle->nodename;

  if ((l = hash_find(handle->node_index, node)) == NULL)
    return 0;

  handle->errnum = GENDERS_ERR_SUCCESS;
  return (list_find_first(l, _is_node, (char *)node)) ? 1 : 0;
}

int 
genders_isattr(genders_t handle, const char *attr) 
{
  if (_loaded_handle_error_check(handle) < 0)
    return -1;

  if (!attr) {
    handle->errnum = GENDERS_ERR_PARAMETERS;
    return -1;
  }

  handle->errnum = GENDERS_ERR_SUCCESS;
  return (list_find_first(handle->attrslist, _is_str, (char *)attr)) ? 1 : 0;
}

int 
genders_isattrval(genders_t handle, const char *attr, const char *val) 
{
  ListIterator itr = NULL;
  genders_node_t n;
  genders_attrval_t av;
  int rv, retval = -1;

  if (_loaded_handle_error_check(handle) < 0)
    goto cleanup;

  if (!attr || !val) {
    handle->errnum = GENDERS_ERR_PARAMETERS;
    goto cleanup;
  }
  
  if (!(itr = list_iterator_create(handle->nodeslist))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  while ((n = list_next(itr))) {
    if (_find_attrval_in_attrlist(handle, n->attrlist, attr, &av) < 0) 
      goto cleanup;
    if (av) {
      if (av->val) { 
        if ((rv = _get_val(handle, n, av)) < 0)
          goto cleanup;

        if ((rv > 0 && !strcmp(handle->valbuf, val)) 
	    || !strcmp(av->val, val)) {
          retval = 1;
          goto cleanup;
        }
      }
    }
  }

  retval = 0;
  handle->errnum = GENDERS_ERR_SUCCESS;
 cleanup:
  if (itr)
    list_iterator_destroy(itr);
  return retval;
}

int 
genders_parse(genders_t handle, const char *filename, FILE *stream) 
{
  int line_count = 1;
  int retval = -1;
  int errcount = 0;
  int rv, ret, fd = -1;
  char buf[GENDERS_READLINE_BUFLEN];
  List debugnodeslist = NULL;
  List debugattrvalslist = NULL;

  if (_handle_error_check(handle) < 0)
    goto cleanup;

  if (!filename)
    filename = GENDERS_DEFAULT_FILE;
  
  if (!stream)
    stream = stderr;

  if ((fd = open(filename,O_RDONLY)) < 0) {
    handle->errnum = GENDERS_ERR_OPEN;
    goto cleanup;
  }

  if (!(debugnodeslist = list_create(_free_genders_node))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  if (!(debugattrvalslist = list_create(NULL))) {
    handle->errnum = GENDERS_ERR_OUTMEM;
    goto cleanup;
  }

  while ((ret = _readline(handle, fd, buf, GENDERS_READLINE_BUFLEN)) > 0) {
    if ((rv = _parse_line(handle, buf, line_count, stream, 
			  debugnodeslist, debugattrvalslist)) < 0) {
      goto cleanup;
    }

    errcount += rv;
    line_count++;
  }

  if (ret < 0 && handle->errnum == GENDERS_ERR_OVERFLOW) {
    fprintf(stderr, "Line %d: exceeds maximum allowed length\n", line_count);
    goto cleanup;
  }

  if (list_count(debugnodeslist) == 0) {
    fprintf(stderr, "No nodes successfully parsed\n");
    goto cleanup;
  }

  retval = errcount;
  handle->errnum = GENDERS_ERR_SUCCESS;
 cleanup:
  close(fd);
  if (debugnodeslist)
    list_destroy(debugnodeslist);
  if (debugattrvalslist)
    list_destroy(debugattrvalslist);
  return retval;
}

void 
genders_set_errnum(genders_t handle, int errnum) 
{
  if (_handle_error_check(handle) < 0)
    return;

  if (errnum >= GENDERS_ERR_SUCCESS && errnum <= GENDERS_ERR_ERRNUMRANGE)
    handle->errnum = errnum;
  else
    handle->errnum = GENDERS_ERR_INTERNAL;
}

/*
 * Various debugging/helper functions which are riddled with
 * corner case bugs.
 */

#ifndef NDEBUG
int
genders_node_index_dump(genders_t handle)
{
  ListIterator itr;
  genders_node_t n;

  itr = list_iterator_create(handle->nodeslist);

  while ((n = list_next(itr)) != NULL) {
    char *nodename = n->name;
    List l = hash_find(handle->node_index, nodename);
    printf("%s: %d\n", nodename, list_count(l));
  }
}
#endif

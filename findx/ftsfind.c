/* find -- search for files in a directory hierarchy (fts version)
   Copyright (C) 1990, 1091, 1992, 1993, 1994, 2000, 2003, 2004, 2005,
   2006, 2007, 2008, 2009, 2010, 2011 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* This file was written by James Youngman, based on oldfind.c.

   GNU find was written by Eric Decker <cire@soe.ucsc.edu>,
   with enhancements by David MacKenzie <djm@gnu.org>,
   Jay Plett <jay@silence.princeton.nj.us>,
   and Tim Wood <axolotl!tim@toad.com>.
   The idea for -print0 and xargs -0 came from
   Dan Bernstein <brnstnd@kramden.acf.nyu.edu>.
*/

/* config.h must always be included first. */
#include <config.h>


/* system headers. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <sys/stat.h>
#include <unistd.h>

/* gnulib headers. */
#include "cloexec.h"
#include "closeout.h"
#include "error.h"
#include "fts_.h"
#include "gettext.h"
#include "progname.h"
#include "quotearg.h"
#include "save-cwd.h"
#include "xgetcwd.h"

/* find headers. */
#include "defs.h"
#include "dircallback.h"
#include "fdleak.h"
#include "unused-result.h"

#define USE_SAFE_CHDIR 1
#undef  STAT_MOUNTPOINTS


#if ENABLE_NLS
# include <libintl.h>
# define _(Text) gettext (Text)
#else
# define _(Text) Text
#define textdomain(Domain)
#define bindtextdomain(Package, Directory)
#endif
#ifdef gettext_noop
# define N_(String) gettext_noop (String)
#else
/* See locate.c for explanation as to why not use (String) */
# define N_(String) String
#endif


/* FTS_TIGHT_CYCLE_CHECK tries to work around Savannah bug #17877
 * (but actually using it doesn't fix the bug).
 */
static int ftsoptions = FTS_NOSTAT|FTS_TIGHT_CYCLE_CHECK|FTS_CWDFD|FTS_VERBATIM;

static int prev_depth = INT_MIN; /* fts_level can be < 0 */
static int curr_fd = -1;


static bool find (int argc, char *argv[]) __attribute_warn_unused_result__;
static bool process_all_startpoints (int argc, char *argv[]) __attribute_warn_unused_result__;



static void
left_dir (void)
{
  if (ftsoptions & FTS_CWDFD)
    {
      if (curr_fd >= 0)
	{
	  close (curr_fd);
	  curr_fd = -1;
	}
    }
  else
    {
      /* do nothing. */
    }
}

/*
 * Signal that we are now inside a directory pointed to by dir_fd.
 * The caller can't tell if this is the first time this happens, so
 * we have to be careful not to call dup() more than once
 */
static void
inside_dir (int dir_fd)
{
  if (ftsoptions & FTS_CWDFD)
    {
      assert (dir_fd == AT_FDCWD || dir_fd >= 0);

      state.cwd_dir_fd = dir_fd;
      if (curr_fd < 0)
	{
	  if (AT_FDCWD == dir_fd)
	    {
	      curr_fd = AT_FDCWD;
	    }
	  else if (dir_fd >= 0)
	    {
	      curr_fd = dup_cloexec (dir_fd);
	    }
	  else
	    {
	      /* curr_fd is invalid, but dir_fd is also invalid.
	       * This should not have happened.
	       */
	      assert (curr_fd >= 0 || dir_fd >= 0);
	    }
	}
    }
  else
    {
      /* FTS_CWDFD is not in use.  We can always assume that
       * AT_FDCWD refers to the directory we are currentl searching.
       *
       * Therefore there is nothing to do.
       */
    }
}



#ifdef STAT_MOUNTPOINTS
static void init_mounted_dev_list (void);
#endif

#define STRINGIFY(X) #X
#define HANDLECASE(N) case N: return #N;

static char *
get_fts_info_name (int info)
{
  static char buf[10];
  switch (info)
    {
      HANDLECASE(FTS_D);
      HANDLECASE(FTS_DC);
      HANDLECASE(FTS_DEFAULT);
      HANDLECASE(FTS_DNR);
      HANDLECASE(FTS_DOT);
      HANDLECASE(FTS_DP);
      HANDLECASE(FTS_ERR);
      HANDLECASE(FTS_F);
      HANDLECASE(FTS_INIT);
      HANDLECASE(FTS_NS);
      HANDLECASE(FTS_NSOK);
      HANDLECASE(FTS_SL);
      HANDLECASE(FTS_SLNONE);
      HANDLECASE(FTS_W);
    default:
      sprintf (buf, "[%d]", info);
      return buf;
    }
}

static void
visit (FTS *p, FTSENT *ent, struct stat *pstat)
{
  struct predicate *eval_tree;

  state.have_stat = (ent->fts_info != FTS_NS) && (ent->fts_info != FTS_NSOK);
  state.rel_pathname = ent->fts_accpath;
  state.cwd_dir_fd   = p->fts_cwd_fd;

  /* Apply the predicates to this path. */
  eval_tree = get_eval_tree ();
  apply_predicate (ent->fts_path, pstat, eval_tree);

  /* Deal with any side effects of applying the predicates. */
  if (state.stop_at_current_level)
    {
      fts_set (p, ent, FTS_SKIP);
    }
}

static const char*
partial_quotearg_n (int n, char *s, size_t len, enum quoting_style style)
{
  if (0 == len)
    {
      return quotearg_n_style (n, style, "");
    }
  else
    {
      char saved;
      const char *result;

      saved = s[len];
      s[len] = 0;
      result = quotearg_n_style (n, style, s);
      s[len] = saved;
      return result;
    }
}


/* We've detected a file system loop.   This is caused by one of
 * two things:
 *
 * 1. Option -L is in effect and we've hit a symbolic link that
 *    points to an ancestor.  This is harmless.  We won't traverse the
 *    symbolic link.
 *
 * 2. We have hit a real cycle in the directory hierarchy.  In this
 *    case, we issue a diagnostic message (POSIX requires this) and we
 *    skip that directory entry.
 */
static void
issue_loop_warning (FTSENT * ent)
{
  if (S_ISLNK(ent->fts_statp->st_mode))
    {
      error (0, 0,
	     _("Symbolic link %s is part of a loop in the directory hierarchy; we have already visited the directory to which it points."),
	     safely_quote_err_filename (0, ent->fts_path));
    }
  else
    {
      /* We have found an infinite loop.  POSIX requires us to
       * issue a diagnostic.  Usually we won't get to here
       * because when the leaf optimisation is on, it will cause
       * the subdirectory to be skipped.  If /a/b/c/d is a hard
       * link to /a/b, then the link count of /a/b/c is 2,
       * because the ".." entry of /a/b/c/d points to /a, not
       * to /a/b/c.
       */
      error (0, 0,
	     _("File system loop detected; "
	       "%s is part of the same file system loop as %s."),
	     safely_quote_err_filename (0, ent->fts_path),
	     partial_quotearg_n (1,
				 ent->fts_cycle->fts_path,
				 ent->fts_cycle->fts_pathlen,
				 options.err_quoting_style));
    }
}

/*
 * Return true if NAME corresponds to a file which forms part of a
 * symbolic link loop.  The command
 *      rm -f a b; ln -s a b; ln -s b a
 * produces such a loop.
 */
static bool
symlink_loop (const char *name)
{
  struct stat stbuf;
  const int rv = options.xstat (name, &stbuf);
  return (0 != rv) && (ELOOP == errno);
}


static void
show_outstanding_execdirs (FILE *fp)
{
  if (options.debug_options & DebugExec)
    {
      int seen=0;
      struct predicate *p;
      p = get_eval_tree ();
      fprintf (fp, "Outstanding execdirs:");

      while (p)
	{
	  const char *pfx;

	  if (pred_is (p, pred_execdir))
	    pfx = "-execdir";
	  else if (pred_is (p, pred_okdir))
	    pfx = "-okdir";
	  else
	    pfx = NULL;
	  if (pfx)
	    {
	      size_t i;
	      const struct exec_val *execp = &p->args.exec_vec;
	      ++seen;

	      fprintf (fp, "%s ", pfx);
	      if (execp->multiple)
		fprintf (fp, "multiple ");
	      fprintf (fp, "%" PRIuMAX " args: ", (uintmax_t) execp->state.cmd_argc);
	      for (i=0; i<execp->state.cmd_argc; ++i)
		{
		  fprintf (fp, "%s ", execp->state.cmd_argv[i]);
		}
	      fprintf (fp, "\n");
	    }
	  p = p->pred_next;
	}
      if (!seen)
	fprintf (fp, " none\n");
    }
  else
    {
      /* No debug output is wanted. */
    }
}

static void
consider_visiting (FTS *p, FTSENT *ent)
{
  struct stat statbuf;
  mode_t mode;
  int ignore, isdir;

  if (options.debug_options & DebugSearch)
    fprintf (stderr,
	     "consider_visiting (early): %s: "
	     "fts_info=%-6s, fts_level=%2d, prev_depth=%d "
	     "fts_path=%s, fts_accpath=%s\n",
	     quotearg_n_style (0, options.err_quoting_style, ent->fts_path),
	     get_fts_info_name (ent->fts_info),
	     (int)ent->fts_level, prev_depth,
	     quotearg_n_style (1, options.err_quoting_style, ent->fts_path),
	     quotearg_n_style (2, options.err_quoting_style, ent->fts_accpath));

  if (ent->fts_info == FTS_DP)
    {
      left_dir ();
    }
  else if (ent->fts_level > prev_depth || ent->fts_level==0)
    {
      left_dir ();
    }
  inside_dir (p->fts_cwd_fd);
  prev_depth = ent->fts_level;

  statbuf.st_ino = ent->fts_statp->st_ino;

  /* Cope with various error conditions. */
  if (ent->fts_info == FTS_ERR
      || ent->fts_info == FTS_DNR)
    {
      nonfatal_target_file_error (ent->fts_errno, ent->fts_path);
      return;
    }
  else if (ent->fts_info == FTS_DC)
    {
      issue_loop_warning (ent);
      error_severity (EXIT_FAILURE);
      return;
    }
  else if (ent->fts_info == FTS_SLNONE)
    {
      /* fts_read() claims that ent->fts_accpath is a broken symbolic
       * link.  That would be fine, but if this is part of a symbolic
       * link loop, we diagnose the problem and also ensure that the
       * eventual return value is nonzero.   Note that while the path
       * we stat is local (fts_accpath), we print the full path name
       * of the file (fts_path) in the error message.
       */
      if (symlink_loop (ent->fts_accpath))
	{
	  nonfatal_target_file_error (ELOOP, ent->fts_path);
	  return;
	}
    }
  else if (ent->fts_info == FTS_NS)
    {
      if (ent->fts_level == 0)
	{
	  /* e.g., nonexistent starting point */
	  nonfatal_target_file_error (ent->fts_errno, ent->fts_path);
	  return;
	}
      else
	{
	  /* The following if statement fixes Savannah bug #19605
	   * (failure to diagnose a symbolic link loop)
	   */
	  if (symlink_loop (ent->fts_accpath))
	    {
	      nonfatal_target_file_error (ELOOP, ent->fts_path);
	      return;
	    }
	  else
	    {
	      nonfatal_target_file_error (ent->fts_errno, ent->fts_path);
	      /* Continue despite the error, as file name without stat info
	       * might be better than not even processing the file name. This
	       * can lead to repeated error messages later on, though, if a
	       * predicate requires stat information.
	       *
	       * Not printing an error message here would be even more wrong,
	       * though, as this could cause the contents of a directory to be
	       * silently ignored, as the directory wouldn't be identified as
	       * such.
	       */
	    }

	}
    }

  /* Cope with the usual cases. */
  if (ent->fts_info == FTS_NSOK
      || ent->fts_info == FTS_NS /* e.g. symlink loop */)
    {
      assert (!state.have_stat);
      assert (ent->fts_info == FTS_NSOK || state.type == 0);
      mode = state.type;
    }
  else
    {
      state.have_stat = true;
      state.have_type = true;
      statbuf = *(ent->fts_statp);
      state.type = mode = statbuf.st_mode;

      if (00000 == mode)
	{
	  /* Savannah bug #16378. */
	  error (0, 0, _("WARNING: file %s appears to have mode 0000"),
		 quotearg_n_style (0, options.err_quoting_style, ent->fts_path));
	}
    }

  /* update state.curdepth before calling digest_mode(), because digest_mode
   * may call following_links().
   */
  state.curdepth = ent->fts_level;
  if (mode)
    {
      if (!digest_mode (&mode, ent->fts_path, ent->fts_name, &statbuf, 0))
	return;
    }

  /* examine this item. */
  ignore = 0;
  isdir = S_ISDIR(mode)
    || (FTS_D  == ent->fts_info)
    || (FTS_DP == ent->fts_info)
    || (FTS_DC == ent->fts_info);

  if (isdir && (ent->fts_info == FTS_NSOK))
    {
      /* This is a directory, but fts did not stat it, so
       * presumably would not be planning to search its
       * children.  Force a stat of the file so that the
       * children can be checked.
       */
      fts_set (p, ent, FTS_AGAIN);
      return;
    }

  if (options.maxdepth >= 0)
    {
      if (ent->fts_level >= options.maxdepth)
	{
	  fts_set (p, ent, FTS_SKIP); /* descend no further */

	  if (ent->fts_level > options.maxdepth)
	    ignore = 1;		/* don't even look at this one */
	}
    }

  if ( (ent->fts_info == FTS_D) && !options.do_dir_first )
    {
      /* this is the preorder visit, but user said -depth */
      ignore = 1;
    }
  else if ( (ent->fts_info == FTS_DP) && options.do_dir_first )
    {
      /* this is the postorder visit, but user didn't say -depth */
      ignore = 1;
    }
  else if (ent->fts_level < options.mindepth)
    {
      ignore = 1;
    }

  if (options.debug_options & DebugSearch)
    fprintf (stderr,
	     "consider_visiting (late): %s: "
	     "fts_info=%-6s, isdir=%d ignore=%d have_stat=%d have_type=%d \n",
	     quotearg_n_style (0, options.err_quoting_style, ent->fts_path),
	     get_fts_info_name (ent->fts_info),
	     isdir, ignore, state.have_stat, state.have_type);

  if (!ignore)
    {
      visit (p, ent, &statbuf);
    }

  if (ent->fts_info == FTS_DP)
    {
      /* we're leaving a directory. */
      state.stop_at_current_level = false;
    }
}



static bool
find (int argc, char *argv[])
{
  char **arglist = (char **)malloc(sizeof(char *) * (argc + 1));
  FTS *p;
  FTSENT *ent;

  inside_dir (AT_FDCWD);

  int arg_max = 0;
  int len;
  for (int i = 0; i < argc; i++)
    {
      arglist[i] = argv[i];
      len = strlen(argv[i]);
      if (arg_max < len) arg_max = len;
    }
  arglist[argc] = NULL;
  state.starting_path_length = arg_max;

  switch (options.symlink_handling)
    {
    case SYMLINK_ALWAYS_DEREF:
      ftsoptions |= FTS_COMFOLLOW|FTS_LOGICAL;
      break;

    case SYMLINK_DEREF_ARGSONLY:
      ftsoptions |= FTS_COMFOLLOW|FTS_PHYSICAL;
      break;

    case SYMLINK_NEVER_DEREF:
      ftsoptions |= FTS_PHYSICAL;
      break;
    }

  if (options.stay_on_filesystem)
    ftsoptions |= FTS_XDEV;

  p = fts_open (arglist, ftsoptions, NULL);
  if (NULL == p)
    {
      error (0, errno, _("cannot search %s"),
        safely_quote_err_filename (0, argv[0]));
      error_severity (EXIT_FAILURE);
    }
  else
    {
      int level = INT_MIN;

      while ( (errno=0, ent=fts_read (p)) != NULL )
	{
	  if (state.execdirs_outstanding)
	    {
	      /* If we changed level, perform any outstanding
	       * execdirs.  If we see a sequence of directory entries
	       * like this: fffdfffdfff, we could build a command line
	       * of 9 files, but this simple-minded implementation
	       * builds a command line for only 3 files at a time
	       * (since fts descends into the directories).
	       */
	      if ((int)ent->fts_level != level)
		{
		  show_outstanding_execdirs (stderr);
		  complete_pending_execdirs ();
		}
	    }
	  level = (int)ent->fts_level;

	  state.already_issued_stat_error_msg = false;
	  state.have_stat = false;
	  state.have_type = !!ent->fts_statp->st_mode;
	  state.type = state.have_type ? ent->fts_statp->st_mode : 0;
	  consider_visiting (p, ent);
	}
      /* fts_read returned NULL; distinguish between "finished" and "error". */
      if (errno)
	{
	  error (0, errno,
		 "failed to read file names from file system at or below %s",
		 safely_quote_err_filename (0, argv[0]));
	  error_severity (EXIT_FAILURE);
	  free(arglist);
	  return false;
	}

      if (0 != fts_close (p))
	{
	  /* Here we break the abstraction of fts_close a bit, because we
	   * are going to skip the rest of the start points, and return with
	   * nonzero exit status.  Hence we need to issue a diagnostic on
	   * stderr. */
	  error (0, errno,
		 _("failed to restore working directory after searching %s"),
		 argv[0]);
	  error_severity (EXIT_FAILURE);
	  free(arglist);
	  return false;
	}
      p = NULL;
    }
  free(arglist);
  return true;
}

// 100 is for now
#define TAKE_NON_EXPRESSION_MAX_PATH_COUNT 100

static
char *my_getline_no_lf(FILE *stream) {
  char *line = NULL;
  size_t n = 0;
  size_t sz;
  if ((sz = getline(&line, &n, stream)) == -1)
    {
      free(line);
      return NULL;
    }

  if (sz >= 1) if (line[sz - 1] == '\r' || line[sz - 1] == '\n') line[sz - 1] = '\0';
  if (sz >= 2) if (line[sz - 2] == '\r' || line[sz - 2] == '\n') line[sz - 2] = '\0';

  return line;
}

static bool
take_non_expression (
  int *out_argc, char *out_argv[TAKE_NON_EXPRESSION_MAX_PATH_COUNT], bool *inout_stdin_mode,
  int *inout_arg_i, int argc, char *argv[],
  bool leading)
{
  // clear prev heap
  int i = 0;
  for (;i < *out_argc; i++) free(out_argv[i]);

  i = 0;
  while (i < TAKE_NON_EXPRESSION_MAX_PATH_COUNT && *inout_arg_i < argc)
    {
      // continue stdin path
      if (*inout_stdin_mode)
        {
          char *line = NULL;
          if ((line = my_getline_no_lf(stdin)) != NULL)
            {
              out_argv[i++] = line;
            }
          else
            {
              // stdin done
              *inout_stdin_mode = false;
              (*inout_arg_i)++;
            }
          continue;
        }

      const char *arg = argv[*inout_arg_i];
      if (arg[0] == '-' && !arg[1])
        {
          // Just '-' is stdin.
          *inout_stdin_mode = true;
          continue;
        }

      if (looks_like_expression(arg, leading)) break;

      char *p = malloc(sizeof(char) * (strlen(arg) + 1));
      strcpy(p, arg);
      out_argv[i++] = p;
      (*inout_arg_i)++;
    }

  *out_argc = i;
  return i > 0;
}


static bool
process_all_startpoints (int argc, char *argv[])
{
  int i;

  int find_argc = 0;
  char *find_argv[TAKE_NON_EXPRESSION_MAX_PATH_COUNT];
  bool stdin_mode = false;

  /* figure out how many start points there are */
  i = 0;
  while (take_non_expression (
    &find_argc, find_argv, &stdin_mode,
    &i, argc, argv,
    true)
  )
    {
      if (!find (find_argc, find_argv)) goto fail;
    }

  if (i == 0)
    {
      /*
       * We use a temporary variable here because some actions modify
       * the path temporarily.  Hence if we use a string constant,
       * we get a coredump.  The best example of this is if we say
       * "find -printf %H" (note, not "find . -printf %H").
       */
      char *defaultpath[] = {"."};
      if (!find (1, defaultpath)) goto fail;
    }

  // success
  for (i = 0; i < find_argc; i++) free(find_argv[i]);
  return true;

fail:
  for (i = 0; i < find_argc; i++) free(find_argv[i]);
  return false;
}




int
main (int argc, char **argv)
{
  int end_of_leading_options = 0; /* First arg after any -H/-L etc. */
  struct predicate *eval_tree;

  if (argv[0])
    set_program_name (argv[0]);
  else
    set_program_name ("find");

  record_initial_cwd ();

  state.already_issued_stat_error_msg = false;
  state.exit_status = 0;
  state.execdirs_outstanding = false;
  state.cwd_dir_fd = AT_FDCWD;

  if (fd_leak_check_is_enabled ())
    {
      remember_non_cloexec_fds ();
    }

  state.shared_files = sharefile_init ("w");
  if (NULL == state.shared_files)
    {
      error (EXIT_FAILURE, errno,
	     _("Failed to initialize shared-file hash table"));
    }

  /* Set the option defaults before we do the locale initialisation as
   * check_nofollow() needs to be executed in the POSIX locale.
   */
  set_option_defaults (&options);

#ifdef HAVE_SETLOCALE
  setlocale (LC_ALL, "");
#endif

  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);
  if (atexit (close_stdout))
    {
      error (EXIT_FAILURE, errno, _("The atexit library function failed"));
    }

  /* Check for -P, -H or -L options.  Also -D and -O, which are
   * both GNU extensions.
   */
  end_of_leading_options = process_leading_options (argc, argv);

  if (options.debug_options & DebugStat)
    options.xstat = debug_stat;

#ifdef DEBUG
  fprintf (stderr, "cur_day_start = %s", ctime (&options.cur_day_start));
#endif /* DEBUG */


  /* We are now processing the part of the "find" command line
   * after the -H/-L options (if any).
   */
  eval_tree = build_expression_tree (argc, argv, end_of_leading_options);

  /* safely_chdir() needs to check that it has ended up in the right place.
   * To avoid bailing out when something gets automounted, it checks if
   * the target directory appears to have had a directory mounted on it as
   * we chdir()ed.  The problem with this is that in order to notice that
   * a file system was mounted, we would need to lstat() all the mount points.
   * That strategy loses if our machine is a client of a dead NFS server.
   *
   * Hence if safely_chdir() and wd_sanity_check() can manage without needing
   * to know the mounted device list, we do that.
   */
  if (!options.open_nofollow_available)
    {
#ifdef STAT_MOUNTPOINTS
      init_mounted_dev_list ();
#endif
    }


  /* process_all_startpoints processes the starting points named on
   * the command line.  A false return value from it means that we
   * failed to restore the original context.  That means it would not
   * be safe to call cleanup() since we might complete an execdir in
   * the wrong directory for example.
   */
  if (process_all_startpoints (argc-end_of_leading_options,
			       argv+end_of_leading_options))
    {
      /* If "-exec ... {} +" has been used, there may be some
       * partially-full command lines which have been built,
       * but which are not yet complete.   Execute those now.
       */
      show_success_rates (eval_tree);
      cleanup ();
    }
  return state.exit_status;
}

bool
is_fts_enabled (int *fts_options)
{
  /* this version of find (i.e. this main()) uses fts. */
  *fts_options = ftsoptions;
  return true;
}

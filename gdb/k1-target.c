/* Target ops to connect to the K1 simulator.

   Copyright (C) 2010, Kalray
*/

#include "config.h"
#include "defs.h"
#include "target.h"

#include <assert.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include "environ.h"
#include "gdbcmd.h"
#include "gdbcore.h"
#include "gdbthread.h"
#include "inferior.h"
#include "observer.h"
#include "osdata.h"
#include "main.h"
#include "symfile.h"
#include "top.h"
#include "arch-utils.h"
#include "cli/cli-decode.h"
#include "cli/cli-setshow.h"
#include "cli/cli-utils.h"

#include "event-top.h"
#include "regcache.h"
#include "event-loop.h"

#include "regcache.h"
#include "elf/k1.h"

#include "elf/k1.h"
#include "k1-target.h"

#ifndef MAX
# define MAX(a, b) ((a < b) ? (b) : (a))
#endif

static cmd_cfunc_ftype *real_run_command;

static struct target_ops k1_target_ops;
static char *da_options = NULL;

static const char *simulation_vehicles[] = { "k1-cluster", "k1-runner", NULL };
static const char *simulation_vehicle;
static const char *scluster_debug_levels[] = {"system", "kernel-user", "user", "inherited", NULL};
static const char *scluster_debug_level;
static const char *sglobal_debug_levels[] = {"system", "kernel-user", "user", NULL};
static const char *sglobal_debug_level;
int idx_global_debug_level, global_debug_level_set;
int opt_hide_threads = 1;

static pid_t server_pid;
static int after_first_resume;
int inf_created_change_th = 0;

static const struct inferior_data *k1_attached_inf_data;

const char *get_str_debug_level (int level)
{
  return scluster_debug_levels[level];
}

static struct inferior_data*
mppa_init_inferior_data (struct inferior *inf)
{
    struct inferior_data *data = xcalloc (1, sizeof (struct inferior_data));
    char *endptr;
    struct osdata *osdata;
    struct osdata_item *last;
    struct osdata_item *item;
    int ix_items;

    data->cluster_debug_level = DBG_LEVEL_INHERITED;
    data->os_supported_debug_level = -1;
    set_inferior_data (inf, k1_attached_inf_data, data);

    /* Cluster name */
    data->cluster = "Cluster ?";

    osdata = get_osdata (NULL);

    for (ix_items = 0;
         VEC_iterate (osdata_item_s, osdata->items,
                      ix_items, item);
         ix_items++) {
        unsigned long pid = strtoul (get_osdata_column (item, "pid"), &endptr, 10);
        const char *cluster = get_osdata_column (item, "cluster");
        
        if (pid != inf->pid) continue;

        data->cluster = cluster;
    }

    return data;
}

int
is_current_k1b_user (void)
{
	int ret = 0;
  
	struct gdbarch *arch = target_gdbarch ();
	if (arch)
		{
			const struct bfd_arch_info *info = gdbarch_bfd_arch_info (arch);
			if (info)
				{
					int mach = info->mach;
					ret = (mach == bfd_mach_k1bdp_usr || mach == bfd_mach_k1bio_usr);
				}
		}
  
	return ret;
}

struct inferior_data*
mppa_inferior_data (struct inferior *inf)
{
    struct inferior_data *data;

    if (is_current_k1b_user ())
      return NULL;
    
    data = inferior_data (inf, k1_attached_inf_data);

    if (!data)
        data = mppa_init_inferior_data (inf);

    return data;
}

static void
k1_push_arch_stratum (struct target_ops *ops, int from_tty)
{   
    if (find_target_beneath (&current_target) != &k1_target_ops) {
        push_target (&k1_target_ops);
    }
}

static void
k1_target_new_thread (struct thread_info *t)
{
    if (!ptid_equal(inferior_ptid, null_ptid))
        inferior_thread ()->step_multi = 1;

    /* /\* When we attach, don't wait... the K1 is already stopped. *\/ */
    /* current_target.to_attach_no_wait = 1; */

    k1_push_arch_stratum (NULL, 0);
}

static void k1_target_mourn_inferior (struct target_ops *target)
{
    struct target_ops *remote_target = find_target_beneath(target);

    gdb_assert (target == &k1_target_ops);
    remote_target->to_mourn_inferior (remote_target);
    /* Force disconnect even if we are in extended mode */
    unpush_target (remote_target);
    unpush_target (&k1_target_ops);

    if (server_pid) {
	kill (server_pid, 9);
        server_pid = 0;
    }
}

static void k1_target_create_inferior (struct target_ops *ops, 
				       char *exec_file, char *args,
				       char **env, int from_tty);

static int 
k1_region_ok_for_hw_watchpoint (struct target_ops *ops, CORE_ADDR addr, int len)
{

	if (is_current_k1b_user ())
		return 0;

    return 1;
}

static void 
k1_target_open (char *name, int from_tty)
{

}

static void k1_target_close (struct target_ops *ops)
{

}

static char *
mppa_pid_to_str (struct target_ops *ops, ptid_t ptid)
{
    struct inferior_data *data;
    struct thread_info *ti;
    struct target_ops *remote_target = find_target_beneath(ops);

    ti = find_thread_ptid (ptid);

    if (ti && !is_current_k1b_user()) {
        const char *extra = remote_target->to_extra_thread_info (ops, ti);
        data = mppa_inferior_data (find_inferior_pid (ptid_get_pid (ptid)));

        if (!extra)
            return xstrdup (data->cluster);
        
        return xstrprintf ("%s of %s", extra, data->cluster);
    } else
        return find_target_beneath (ops)->to_pid_to_str (find_target_beneath (ops), ptid);
}

static char *
mppa_threads_extra_info (struct target_ops *ops, struct thread_info *tp)
{
    return NULL;
}

static void k1_target_attach (struct target_ops *ops, char *args, int from_tty)
{
    const char tar_remote_str[] = "target extended-remote";
    int saved_batch_silent = batch_silent;
    struct observer *new_thread_observer;
    int print_thread_events_save = print_thread_events;
    char *host, *port, *tar_remote_cmd;
    volatile struct gdb_exception ex;

    if (!args)
        args = "";

    port = strchr (args, ':');
    if (port) {
        *port = 0;
        port++;
        host = args;
    } else {
        port = args;
        host = "";
    }

    tar_remote_cmd = alloca (strlen(host) + strlen(port) + strlen(tar_remote_str) + 4);
    parse_pid_to_attach (port);

    print_thread_events = 0;
    sprintf (tar_remote_cmd, "%s %s:%s", tar_remote_str, host, port);

    /* Load the real debug target by issuing 'target remote'. Of
       course things aren't that simple because it's not meant to be
       used that way. One issue is that connecting to the gdb_stub
       will emit MI stop notifications and will print the initial
       frame at the connection point. BATCH_SILENT removes the frame
       display (see infrun.c) and the ugly hack with the observer
       makes infrun believe that we're in a series of steps and thus
       inhibits the emission of the new_thread observer notification
       that prints the initial MI *stopped message. */
    batch_silent = 1;
    new_thread_observer = observer_attach_new_thread (k1_target_new_thread);
    /* tar remote */

    TRY_CATCH (ex, RETURN_MASK_ALL)
    {
        execute_command (tar_remote_cmd, 0);
    }
    if (ex.reason < 0)
    {
        observer_detach_new_thread (new_thread_observer);
        batch_silent = saved_batch_silent;
        print_thread_events = print_thread_events_save;
        throw_exception (ex);
    }

    /* We need to tell the debugger to fake a synchronous
       command. This has already been done at the upper level when the
       main loop executes the "run" command, but the execute_command
       call we did just above reseted to async handling when it
       terminated. */ 
    async_disable_stdin ();
    k1_push_arch_stratum (NULL, 0);

    /* Remove hacks*/
    observer_detach_new_thread (new_thread_observer);
    batch_silent = saved_batch_silent;
    inferior_thread ()->step_multi = 0;
    current_inferior ()->control.stop_soon = NO_STOP_QUIETLY;
    print_thread_events = print_thread_events_save;
}

static void k1_target_create_inferior (struct target_ops *ops, 
				       char *exec_file, char *args,
				       char **env, int from_tty)
{
    char set_non_stop_cmd[] = "set non-stop";
    char set_pagination_off_cmd[] = "set pagination off";
    char **argv_args = gdb_buildargv (args);
    char **da_args = gdb_buildargv (da_options);
    char **stub_args;
    char **arg;
    int nb_args = 0, nb_da_args = 0;
    int pipefds[2];
    int no_march = 0;
    int no_mcluster = 0;
    int port;
    int core;
    int argidx = 0;
    struct bound_minimal_symbol pthread_create_sym;
    struct bound_minimal_symbol rtems_task_start_sym;

    if (exec_file == NULL)
	error (_("No executable file specified.\n\
Use the \"file\" or \"exec-file\" command."));

    pthread_create_sym = lookup_minimal_symbol_text ("pthread_create", NULL);
    rtems_task_start_sym = lookup_minimal_symbol_text ("rtems_task_start", NULL);

    if (pthread_create_sym.minsym || rtems_task_start_sym.minsym) {
	execute_command (set_non_stop_cmd, 0);
	execute_command (set_pagination_off_cmd, 0);
    }

    arg = argv_args;
    while (arg && *arg++) nb_args++;
    arg = da_args;
    while (arg && *arg++) nb_da_args++;

    stub_args = xmalloc ((nb_args+nb_da_args+6)*sizeof (char*));
    stub_args[argidx++] = (char *) simulation_vehicle;

    core = (elf_elfheader(exec_bfd)->e_flags & ELF_K1_CORE_MASK);

    if (nb_da_args && strlen (da_options)) {
	arg = da_args;
	while (*arg) {
	    if (strncmp (*arg, "--mcluster=", 11) == 0)
		no_mcluster = 1;
	    if (strncmp (*arg, "--march=", 8) == 0)
		no_march = 1;


	    stub_args[argidx++] = *arg++;
	}
    }

    if(!no_march)
      switch(core) {
	case ELF_K1_CORE_DP:          
	case ELF_K1_CORE_IO:
	    stub_args[argidx++] = "--march=andey";
	    break;

	case ELF_K1_CORE_B_DP:
	case ELF_K1_CORE_B_IO:
	    stub_args[argidx++] = "--march=bostan";
	    break;
	default:
	    error (_("The K1 binary is compiled for an unknown core."));
      }

    if (!no_mcluster)
	switch (core) {
	case ELF_K1_CORE_B_DP:
	case ELF_K1_CORE_DP:          
	    stub_args[argidx++] = "--mcluster=node";
	    break;

	case ELF_K1_CORE_B_IO:
	case ELF_K1_CORE_IO:
	    stub_args[argidx++] = "--mcluster=ioddr";
	    break;

	default:
	    error (_("The K1 binary is compiled for an unknown core."));
	}

    stub_args[argidx++] = "--gdb";
    stub_args[argidx++] = "--";
    stub_args[argidx++] = exec_file;
    memcpy (stub_args + argidx,  argv_args, (nb_args+1)*sizeof (char*));

    /* Check that we didn't overflow the allocation above. */
    gdb_assert (argidx < nb_args+nb_da_args+6);

    if (server_pid != 0) {
	kill (server_pid, 9);
	waitpid (server_pid, NULL, 0);
    }

    pipe (pipefds);
    server_pid = fork ();
    
    if (server_pid < 0)
	error ("Couldn't fork to launch the server.");

    if (server_pid == 0) {
	char path[PATH_MAX];
	char tmp[PATH_MAX] = { 0 };
	char *dir;

	close (pipefds[0]);
	dup2(pipefds[1], 500);
	close (pipefds[1]);

	setsid ();

	/* Child */
	if (env)
	    environ = env;
	execvp (simulation_vehicle, stub_args);
	
	/* Not in PATH */
	if (readlink ("/proc/self/exe", tmp, PATH_MAX) != -1) {
	    dir = dirname (tmp);
	    snprintf (path, PATH_MAX, "%s/%s", dir, simulation_vehicle);
	    execvp (path, stub_args);
	}

	printf_unfiltered ("Could not find %s in you PATH\n", simulation_vehicle);
	exit (1);
    } else {
	int port;
	char cmd_port[10];
	
	close (pipefds[1]);
	read (pipefds[0], &port, sizeof(port));
	close (pipefds[0]);
	
	sprintf (cmd_port, "%i", port);
	k1_target_attach (ops, cmd_port, from_tty);
    }

}

static int
mppa_mark_clusters_booted (struct inferior *inf, void *_ptid)
{
    struct thread_info *thread = any_live_thread_of_process (inf->pid);
    ptid_t *ptid = _ptid;

    /* Newer GDBs mark the thread as running before passing it to
       target_resume. However, if we are resuming the thread, it must
       have been stooped before... */
    if (thread && (is_stopped (thread->ptid)
	           || ptid_match (thread->ptid, *ptid))) {
        mppa_inferior_data (inf)->booted = 1;
    }
    return 0;
}

static void
mppa_target_resume (struct target_ops *ops,
                    ptid_t ptid, int step, enum gdb_signal siggnal)
{
    struct target_ops *remote_target = find_target_beneath(ops);

    if (!after_first_resume && !is_current_k1b_user()) {
        after_first_resume = 1;
        iterate_over_inferiors (mppa_mark_clusters_booted, &ptid);
    }
    
    return remote_target->to_resume (remote_target, ptid, step, siggnal);
}

static ptid_t
k1_target_wait (struct target_ops *target,
                ptid_t ptid, struct target_waitstatus *status, int options)
{
    struct target_ops *remote_target = find_target_beneath(target);
    ptid_t res;
    struct inferior *inferior;
    int ix_items;

    res = remote_target->to_wait (remote_target, ptid, status, options);
	if(is_current_k1b_user())
		return res;

    if (!after_first_resume)
        return res;

    inferior = find_inferior_pid (ptid_get_pid (res));

    if (inferior && !mppa_inferior_data (inferior)->booted) {
        char *endptr;
        struct osdata *osdata;
        struct osdata_item *last;
        struct osdata_item *item;

        osdata = get_osdata (NULL);
        
        for (ix_items = 0;
             VEC_iterate (osdata_item_s, osdata->items,
                          ix_items, item);
             ix_items++) {
            unsigned long pid = strtoul (get_osdata_column (item, "pid"), &endptr, 10);
            const char *file = get_osdata_column (item, "command");
            const char *cluster = get_osdata_column (item, "cluster");
            int os_debug_level;
            struct inferior_data *data;
         
            if (pid != ptid_get_pid (res))
                continue;

            data = mppa_inferior_data (inferior);
            data->booted = 1;
            
            //if os support a debug level, set it to the cluster
            os_debug_level = get_os_supported_debug_levels (inferior);
            if (os_debug_level > DBG_LEVEL_SYSTEM && !global_debug_level_set &&
              data->cluster_debug_level == DBG_LEVEL_INHERITED)
            {
              set_cluster_debug_level_no_check (inferior, os_debug_level);
            }
            //continue_if_os_supports_debug_level (inferior);

            if (file && file[0] != 0) {
                status->kind = TARGET_WAITKIND_EXECD;
                status->value.execd_pathname = (char *) file;
            } else {
                printf_filtered ("[ %s booted ]\n", cluster);
                status->kind = TARGET_WAITKIND_SPURIOUS;
            }
            
            break;                
        }

    }

    return res;
}

static void
mppa_attach (struct target_ops *ops, const char *args, int from_tty)
{
    struct target_ops *remote_target, *k1_ops = find_target_beneath(&current_target);

    if (k1_ops != &k1_target_ops)
        error ("Don't know how to attach.  Try \"help target\".");

    remote_target = find_target_beneath(&k1_target_ops);
    return remote_target->to_attach (remote_target, args, from_tty);
}

static int
k1_target_can_run (struct target_ops *ops)
{
    return 1;
}

static int
k1_target_supports_non_stop (struct target_ops *ops)
{
  return 1;
}

static int
k1_target_can_async (struct target_ops *ops)
{
  return target_async_permitted;
}

static void change_thread_cb (struct inferior *inf)
{
  struct thread_info *th;
  
  //printf ("change thread_cb pid=%d, lpw=%d\n", inferior_ptid.pid, inferior_ptid.lwp);

  if (is_stopped (inferior_ptid))
    return;
  
  th = any_live_thread_of_process (inf->pid);
  if (!is_stopped (th->ptid))
      return;

  switch_to_thread (th->ptid);
}

static int
is_crt_cpu_in_user_mode (struct regcache *regcache)
{
  uint64_t ps;
  regcache_raw_read_unsigned (regcache, 65, &ps);

  return ((ps & 1) == 0); //PS.PM == 0 (user mode)
}

static void
k1_fetch_registers (struct target_ops *target, struct regcache *regcache, int regnum)
{
  // don't use current_inferior () & current_inferior_
  // our caller (regcache_raw_read) changes only inferior_ptid
  
  struct target_ops *remote_target;
  int crt_thread_mode_used, new_mode;
  struct inferior_data *data;
  struct inferior *inf;
  
  // get the registers of the current thread (CPU) in the usual way
  remote_target = find_target_beneath (target);
  remote_target->to_fetch_registers (target, regcache, regnum);

  if (is_current_k1b_user ())
    return;

  // first time we see a cluster, set the debug level
  inf = find_inferior_pid (inferior_ptid.pid);
  data = mppa_inferior_data (inf);
  if (!data)
    return;

  if (data->cluster_debug_level_postponed)
  {
    data->cluster_debug_level_postponed = 0;
    send_cluster_debug_level (get_debug_level (inferior_ptid.pid));
  }

  // after attach, if "c -a" executed to skip system code because of the
  // debug level, change the thread to a stopped one
  if (inf_created_change_th)
  {
    if (is_crt_cpu_in_user_mode (regcache))
    {
      inf_created_change_th = 0;
      create_timer (0, (void (*)(void*)) change_thread_cb, inf);
    }
  }
}

static struct cmd_list_element *kalray_set_cmdlist;
static struct cmd_list_element *kalray_show_cmdlist;

static void
set_kalray_cmd (char *args, int from_tty)
{
  help_list (kalray_set_cmdlist, "set kalray ", -1, gdb_stdout);
}

static void
show_kalray_cmd (char *args, int from_tty)
{
  help_list (kalray_show_cmdlist, "show kalray ", -1, gdb_stdout);
}

static int get_str_sym_sect (char *elf_file, int offset, char **sret)
{
  asection *s;
  int size_sret;
  bfd *hbfd = bfd_openr (elf_file, NULL);
  if (hbfd == NULL)
    return 0;

  size_sret = 200;
  *sret = (char *) malloc (size_sret);
  sprintf (*sret, "0x%08x ", offset);
  
  //check if the file is in format
  if (!bfd_check_format (hbfd, bfd_object))
  {
    if (bfd_get_error () != bfd_error_file_ambiguously_recognized)
    {
      printf ("Incompatible file format %s\n", elf_file);
      bfd_close (hbfd);
      return 0;
    }
  }

  for (s = hbfd->sections; s; s = s->next)
  {
    if (bfd_get_section_flags (hbfd, s) & SEC_ALLOC)
    {
      const char *sname = bfd_section_name (hbfd, s);
      if (strlen (*sret) + strlen (sname) + 30 > size_sret)
      {
        size_sret += 100;
        *sret = (char *) realloc (*sret, size_sret);
      }
      if (!strcmp (sname , ".text"))
      {
        sprintf (*sret, "0x%08x ",
          (unsigned int) (bfd_section_lma (hbfd, s) + offset));
      }
      else
        sprintf (*sret + strlen (*sret), "-s %s 0x%08x ", sname,
          (unsigned int) (bfd_section_lma (hbfd, s) + offset));
    }
  }
  
  bfd_close (hbfd);

  return 1;
}



static int str_debug_level_to_idx (const char *slevel)
{
  int level;
  for (level = 0; level < DBG_LEVEL_MAX; level++)
    if (!strcmp (slevel, scluster_debug_levels[level]))
      return level;

  return 0;
}

static void apply_cluster_debug_level (struct inferior *inf)
{
  int level;
  int os_supported_level;
  struct thread_info *th;
  ptid_t save_ptid, th_ptid;

  if (is_current_k1b_user ())
    return;

  level = get_debug_level (inf->pid);
  
  th = any_live_thread_of_process (inf->pid);
  //the CPU must be stopped
  if (th == NULL || th->state != THREAD_STOPPED)
  {
    struct inferior_data *data = mppa_inferior_data (inf);
    //printf ("Info: No stopped CPU found for %s. "
    //  "Postpone the setting of the cluster debug level.\n", data->cluster);
    data->cluster_debug_level_postponed = 1;
    send_cluster_postponed_debug_level (inf, level);
    return;
  }

  os_supported_level = get_os_supported_debug_levels (inf);
  if (level > os_supported_level)
    return;

  save_ptid = inferior_ptid;
  th_ptid = th->ptid;
  switch_to_thread (th_ptid);
  set_general_thread (th_ptid);

  send_cluster_debug_level (level);

  switch_to_thread (save_ptid);
  set_general_thread (save_ptid);
}

static void
attach_user_command (char *args, int from_tty)
{
	static const char *syntax = "Syntax: attach-user <comm> [<k1_user_mode_program>]\n";
	char *file, *comm, *pargs, *cmd;
	int saved_batch_silent = batch_silent;
	int print_thread_events_save = print_thread_events;
	volatile struct gdb_exception ex;

	if (!ptid_equal (inferior_ptid, null_ptid))
		{
			printf ("Gdb already attached!\n");
			return;
		}
  
	pargs = args;
	comm = extract_arg (&pargs);
	if (!comm)
		{
			printf ("Error: the comm was not specified!\n%s", syntax);
			return;
		}
	file = extract_arg (&pargs);

	cmd = (char *) malloc (MAX (strlen (comm),
								(file != NULL) ? strlen (file) : 0) + 100);
  
	execute_command ("set pagination off", 0);

	sprintf (cmd, "target remote %s", comm);
	batch_silent = 1;
	print_thread_events_save = 0;
  
	TRY_CATCH (ex, RETURN_MASK_ALL)
		{
			execute_command (cmd, 0);
		}
	if (ex.reason < 0)
		{
			printf ("Error while trying to connect (%s).\n", ex.message ?: "");
			goto end;
		}
	k1_push_arch_stratum (NULL, 0);
	printf ("Attached to user debug using %s.\n", comm);
  
	if (file)
	{
    struct stat s;
    if (stat( file, &s) != 0)
    printf ("Cannot find file %s: %s.\n", file, strerror(errno));
    else
    {
      char *ssect;
      if (get_str_sym_sect (file, 0x6000000, &ssect))
      {
        cmd = (char *) realloc (cmd, strlen (cmd) + strlen (ssect) + 100);
        sprintf (cmd, "add-symbol-file %s %s", file, ssect);
        free (ssect);

        TRY_CATCH (ex, RETURN_MASK_ALL)
        {
          execute_command (cmd, 0);
        }
        if (ex.reason < 0)
        {
          printf ("Error while trying to add symbol file %s (%s).\n", file, ex.message ?: "");
          goto end;
        }
      }
      else
        printf ("Cannot load file %s.\n", file);
    }
  }
  
 end:
	batch_silent = saved_batch_silent;
	print_thread_events = print_thread_events_save;
	free (comm);
	free (cmd);
	if (file)
		free (file);
}

void set_cluster_debug_level_no_check (struct inferior *inf, int debug_level)
{
  struct inferior_data *data;

  if (is_current_k1b_user ())
    return;

  data = mppa_inferior_data (inf);
  data->cluster_debug_level = debug_level; 
  apply_cluster_debug_level (inf);
}

static void set_cluster_debug_level (char *args, int from_tty, struct cmd_list_element *c)
{
  int new_level, prev_level;
  struct inferior *inf;

  if (ptid_equal (inferior_ptid, null_ptid))
    error (_("Cannot set debug level without a selected thread."));

  if (is_current_k1b_user ())
  {
    printf ("Linux user mode does not have a debug level\n");
    return;
  }

  inf = current_inferior ();
  prev_level = get_cluster_debug_level (inf->pid);
  new_level = str_debug_level_to_idx (scluster_debug_level);

  if (new_level != prev_level)
  {
    int os_supported_level = get_os_supported_debug_levels (inf);
    if (os_supported_level < new_level)
    {
      struct inferior_data *data = mppa_inferior_data (inf);
      printf ("Cannot set debug level %s for %s (highest level supported by os is %s).\n",
        scluster_debug_levels[new_level], data->cluster,
        scluster_debug_levels[os_supported_level]);
    }
    else
      set_cluster_debug_level_no_check (inf, new_level);
  }
}

static int set_cluster_debug_level_iter (struct inferior *inf, void *not_used)
{
  apply_cluster_debug_level (inf);
  return 0;
}

void apply_global_debug_level (int level)
{
  if (is_current_k1b_user ())
    return;

  idx_global_debug_level = level;
  if (have_inferiors ())
    iterate_over_inferiors (set_cluster_debug_level_iter, NULL);
  else
  {
    printf ("Info: No cluster found. Action postponed for attach.\n");
  }
}

static void set_global_debug_level (char *args, int from_tty, struct cmd_list_element *c)
{
  int new_level;

  if (is_current_k1b_user ())
  {
    printf ("Linux user mode does not have a debug level\n");
    return;
  }

  new_level = str_debug_level_to_idx (sglobal_debug_level);
  global_debug_level_set = 1;
  if (new_level != idx_global_debug_level) 
  {
    apply_global_debug_level (new_level);
  }
}

static void
show_cluster_debug_level (struct ui_file *file, int from_tty, 
  struct cmd_list_element *c, const char *value)
{
  struct inferior_data *data;
  int level;

  if (ptid_equal (inferior_ptid, null_ptid) || is_exited (inferior_ptid))
  {
    printf (_("Cannot show debug level without a live selected thread."));
    return;
  }

  if (is_current_k1b_user ())
  {
    printf ("Linux user mode does not have a debug level\n");
    return;
  }

  data = mppa_inferior_data (find_inferior_pid (inferior_ptid.pid));
  
  level = get_cluster_debug_level (-1);
  fprintf_filtered (file, "The cluster debug level is \"%s\"%s.\n",
    scluster_debug_levels[level], 
    data->cluster_debug_level_postponed ? " (setting postponed)" : "");
}

static void
show_global_debug_level (struct ui_file *file, int from_tty, 
  struct cmd_list_element *c, const char *value)
{
  if (is_current_k1b_user ())
  {
    printf ("Linux user mode does not have a debug level\n");
    return;
  }

  fprintf_filtered (file, "The global debug level  is \"%s\".\n",
    scluster_debug_levels[idx_global_debug_level]);
}

extern int remote_hw_breakpoint_limit;
extern int remote_hw_watchpoint_limit;

static void
attach_mppa_command (char *args, int from_tty)
{
    char set_non_stop_cmd[] = "set non-stop";
    char set_pagination_off_cmd[] = "set pagination off";
    struct osdata *osdata;
    struct osdata_item *last;
    struct osdata_item *item;
    int ix_items,cur_pid, bstopped, bcur_inf_stopped;
    struct inferior *cur_inf;
    ptid_t cur_ptid, stopped_ptid;

    dont_repeat ();

    print_thread_events = 0;
    after_first_resume = 0;

    k1_push_arch_stratum (NULL, 0);
    execute_command (set_non_stop_cmd, 0);
    execute_command (set_pagination_off_cmd, 0);

    k1_target_attach (&current_target, args, from_tty);

    remote_hw_breakpoint_limit = 0;
    remote_hw_watchpoint_limit = 1;

	bstopped = 0;
	bcur_inf_stopped = 0;
    osdata = get_osdata (NULL);
    
    cur_inf = current_inferior();
    set_inferior_data (cur_inf, k1_attached_inf_data, NULL);
    cur_ptid = inferior_ptid;
    cur_pid = cur_inf->pid;

    for (ix_items = 0;
         VEC_iterate (osdata_item_s, osdata->items,
                      ix_items, item);
         ix_items++) {
        char *endptr;
        unsigned long pid = strtoul (get_osdata_column (item, "pid"), &endptr, 10);
        struct inferior *inf;
        char attach_cmd[25];
        
        if (pid == cur_inf->pid) {
            continue;
        }
        
        inf = add_inferior_with_spaces ();
        set_current_inferior (inf);
        switch_to_thread (null_ptid);
        set_current_program_space (inf->pspace);
        sprintf (attach_cmd, "attach %li&", pid);
        execute_command (attach_cmd, 0);
		inf->control.stop_soon = NO_STOP_QUIETLY;
    }

    bstopped = 0;
    bcur_inf_stopped = 0;
    osdata = get_osdata (NULL);

    for (ix_items = 0;
         VEC_iterate (osdata_item_s, osdata->items,
                      ix_items, item);
         ix_items++) {
        char *endptr;
        struct thread_info *live_th;
        unsigned long pid = strtoul (get_osdata_column (item, "pid"), &endptr, 10);
        const char *file = get_osdata_column (item, "command");
        const char *running = get_osdata_column (item, "running");

        if (strcmp (running, "yes"))
            continue;

        live_th = any_live_thread_of_process (pid);
        if (live_th && !live_th->stop_requested)
            set_stop_requested (live_th->ptid, 1);

        if (pid == cur_pid)
            bcur_inf_stopped = 1;

        if (!bstopped)
        {
            bstopped = 1;
            stopped_ptid = any_live_thread_of_process (pid)->ptid;
        }

        if (file && file[0]) {
            switch_to_thread (any_thread_of_process (pid)->ptid);
            exec_file_attach ((char *) file, 0);
            symbol_file_add_main (file, 0);
        }
    }
    if (!bstopped)
        async_enable_stdin ();

    if (!bstopped || bcur_inf_stopped)
        switch_to_thread (cur_ptid);
    else
        switch_to_thread (stopped_ptid);

    /* This is a hack to populate the dwarf mapping tables now that we
       have the architecture at hand. Having these tables initialized
       from the debug reader routines will break as objfile_arch won't
       have the register descriptions */
    gdbarch_dwarf2_reg_to_regnum (get_current_arch(), 0);
    
    if (idx_global_debug_level)
    {
      apply_global_debug_level (idx_global_debug_level);
    }
}

static void
run_mppa_command (char *args, int from_tty)
{
    char set_non_stop_cmd[] = "set non-stop";
    char set_pagination_off_cmd[] = "set pagination off";
    char run_cmd[] = "run";

    dont_repeat ();

    k1_push_arch_stratum (NULL, 0);
    execute_command (set_non_stop_cmd, 0);
    execute_command (set_pagination_off_cmd, 0);
    execute_command (run_cmd, 0);
}

static void
mppa_inferior_data_cleanup (struct inferior *inf, void *data)
{
    xfree (data);
}

static void mppa_observer_breakpoint_created (struct breakpoint *b)
{
  if (is_current_k1b_user ())
    return;

  if (b && b->addr_string && !strcmp (b->addr_string, "main"))
  {
    send_stop_at_main (0);
  }

}

static void mppa_observer_breakpoint_deleted (struct breakpoint *b)
{
  if (is_current_k1b_user ())
    return;

  if (b && b->addr_string && !strcmp (b->addr_string, "main"))
  {
    send_stop_at_main (1);
  }
}

void
_initialize__k1_target (void)
{
    simulation_vehicle = simulation_vehicles[0];
    scluster_debug_level = scluster_debug_levels[DBG_LEVEL_INHERITED];
    idx_global_debug_level = DBG_LEVEL_SYSTEM;
    global_debug_level_set = 0;
    sglobal_debug_level = sglobal_debug_levels[idx_global_debug_level];
    
    k1_target_ops.to_shortname = "mppa";
    k1_target_ops.to_longname = "Kalray MPPA connection";
    k1_target_ops.to_doc = 
	"Connect to a Kalray MPPA execution vehicle.";
    k1_target_ops.to_stratum = arch_stratum;

    k1_target_ops.to_open = k1_target_open;
    k1_target_ops.to_close = k1_target_close;

    k1_target_ops.to_create_inferior = k1_target_create_inferior;
    k1_target_ops.to_attach = mppa_attach;
    k1_target_ops.to_mourn_inferior = k1_target_mourn_inferior;
    k1_target_ops.to_wait = k1_target_wait;
    k1_target_ops.to_resume = mppa_target_resume;

    k1_target_ops.to_supports_non_stop = k1_target_supports_non_stop;
    k1_target_ops.to_can_async_p = k1_target_can_async;
    k1_target_ops.to_can_run = k1_target_can_run;
    k1_target_ops.to_attach_no_wait = 0;
    k1_target_ops.to_region_ok_for_hw_watchpoint = k1_region_ok_for_hw_watchpoint;

    k1_target_ops.to_pid_to_str = mppa_pid_to_str;
    k1_target_ops.to_extra_thread_info = mppa_threads_extra_info;

    k1_target_ops.to_fetch_registers = k1_fetch_registers;
    
    k1_target_ops.to_magic = OPS_MAGIC;
    
    add_target (&k1_target_ops);

    {
      extern int (*p_kalray_hide_thread) (struct thread_info *tp, ptid_t crt_ptid);
      p_kalray_hide_thread = kalray_hide_thread;
    }

    add_prefix_cmd ("kalray", class_maintenance, set_kalray_cmd, _("\
Kalray specific variables\n				    \
Configure various Kalray specific variables."),
		    &kalray_set_cmdlist, "set kalray ",
		    0 /* allow-unknown */, &setlist);
    add_prefix_cmd ("kalray", class_maintenance, show_kalray_cmd, _("\
Kalray specific variables\n				    \
Configure various Kalray specific variables."),
		    &kalray_show_cmdlist, "show kalray ",
		    0 /* allow-unknown */, &showlist);

    add_setshow_string_noescape_cmd ("debug_agent_options", class_maintenance,
				     &da_options,  _("\
Set the options passed to the debug agent."), _("\
Show the options passed to the debug agent."), NULL, NULL, NULL,
				   &kalray_set_cmdlist, &kalray_show_cmdlist);

    add_setshow_enum_cmd ("simulation_vehicle", class_maintenance,
			  simulation_vehicles, &simulation_vehicle, _("\
Set the simulation vehicle to use for execution."), _("\
Show the simulation vehicle to use for execution."), NULL, NULL, NULL,
			  &kalray_set_cmdlist, &kalray_show_cmdlist);

    add_setshow_enum_cmd ("cluster_debug_level", class_maintenance,
			  scluster_debug_levels, &scluster_debug_level,
        _("Set the cluster debug level."), _("Show the cluster debug level."), 
        NULL, set_cluster_debug_level, show_cluster_debug_level,
			  &kalray_set_cmdlist, &kalray_show_cmdlist);
    
    add_setshow_enum_cmd ("global_debug_level", class_maintenance,
			  sglobal_debug_levels, &sglobal_debug_level,
        _("Set the global debug level."), _("Show the global debug level."), 
        NULL, set_global_debug_level, show_global_debug_level,
			  &kalray_set_cmdlist, &kalray_show_cmdlist);
    
    add_setshow_boolean_cmd ("hide_threads", class_maintenance, &opt_hide_threads,
      _("Set hide threads in debug level."), _("Show hide threads in debug level."),
      NULL, NULL, NULL, &kalray_set_cmdlist, &kalray_show_cmdlist);
    
    add_com ("attach-mppa", class_run, attach_mppa_command, _("\
Connect to a MPPA TLM platform and start debugging it.\n\
Usage is `attach-mppa PORT'."));
    add_com ("attach-user", class_run, attach_user_command,
			 _("Connect to gdbserver running on MPPA to make user mode debug.\n"
			   "Usage is `attach-user <comm> [<k1_user_mode_program>]'."));

    add_com ("run-mppa", class_run, run_mppa_command, _("\
Connect to a MPPA TLM platform and start debugging it."));


    observer_attach_inferior_created (k1_push_arch_stratum);
    k1_attached_inf_data = register_inferior_data_with_cleanup (NULL, mppa_inferior_data_cleanup);
    
    observer_attach_breakpoint_created (mppa_observer_breakpoint_created);
    observer_attach_breakpoint_deleted (mppa_observer_breakpoint_deleted);
}

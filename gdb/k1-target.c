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
#include "elf/k1.h"

#include "cli/cli-decode.h"
#include "cli/cli-setshow.h"

static cmd_cfunc_ftype *real_run_command;

static struct target_ops k1_target_ops;
static char *da_options = NULL;

static const char *simulation_vehicles[] = { "k1-cluster", "k1-runner", NULL };
static const char *simulation_vehicle;

static pid_t server_pid;
static int after_first_resume;

static const struct inferior_data *k1_attached_inf_data;

struct inferior_data {
    const char *cluster;
    int booted;
};

static struct inferior_data*
mppa_init_inferior_data (struct inferior *inf)
{
    struct inferior_data *data = xcalloc (1, sizeof (struct inferior_data));
    char *endptr;
    struct osdata *osdata;
    struct osdata_item *last;
    struct osdata_item *item;
    int ix_items;

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

static struct inferior_data*
mppa_inferior_data (struct inferior *inf)
{
    struct inferior_data *data = inferior_data (inf, k1_attached_inf_data);

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

    if (ti) {
        char *extra = remote_target->to_extra_thread_info (ops, ti);
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

void k1_target_attach (struct target_ops *ops, char *args, int from_tty)
{
    const char tar_remote_str[] = "target extended-remote";
    int saved_batch_silent = batch_silent;
    struct observer *new_thread_observer;
    int print_thread_events_save = print_thread_events;
    char *host, *port, *tar_remote_cmd;

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
    execute_command (tar_remote_cmd, 0);
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
    int no_mcore = 0;
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
    stub_args[argidx++] = simulation_vehicle;

    core = (elf_elfheader(exec_bfd)->e_flags & ELF_K1_CORE_MASK);

    if (nb_da_args && strlen (da_options)) {
	arg = da_args;
	while (*arg) {
	    if (strncmp (*arg, "--mmppa=", 8) == 0
		|| strncmp (*arg, "--mcluster=", 11) == 0)
		no_mcore = 1;

	    stub_args[argidx++] = *arg++;
	}
    }

    if (!no_mcore)
	switch (core) {
	case ELF_K1_CORE_DP:          
	    stub_args[argidx++] = "--mcluster=node";
	    break;
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
	dup2(pipefds[1], 100);
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

    if (!after_first_resume) {
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
         
            if (pid != ptid_get_pid (res))
                continue;

            mppa_inferior_data (inferior)->booted = 1;

            if (file && file[0] != 0) {
                status->kind = TARGET_WAITKIND_EXECD;
                status->value.execd_pathname = file;
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

static void
attach_mppa_command (char *args, int from_tty)
{
    char set_non_stop_cmd[] = "set non-stop";
    char set_pagination_off_cmd[] = "set pagination off";
    struct osdata *osdata;
    struct osdata_item *last;
    struct osdata_item *item;
    int ix_items;
    struct inferior *cur_inf;
    ptid_t cur_ptid;

    dont_repeat ();

    print_thread_events = 0;
    after_first_resume = 0;

    k1_push_arch_stratum (NULL, 0);
    execute_command (set_non_stop_cmd, 0);
    execute_command (set_pagination_off_cmd, 0);

    k1_target_attach (&current_target, args, from_tty);

    osdata = get_osdata (NULL);
    
    cur_inf = current_inferior();
    set_inferior_data (cur_inf, k1_attached_inf_data, NULL);
    cur_ptid = inferior_ptid;

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

    for (ix_items = 0;
         VEC_iterate (osdata_item_s, osdata->items,
                      ix_items, item);
         ix_items++) {
        char *endptr;
        unsigned long pid = strtoul (get_osdata_column (item, "pid"), &endptr, 10);
        const char *file = get_osdata_column (item, "command");
        const char *running = get_osdata_column (item, "running");

        if (strcmp (running, "yes"))
            continue;

        if (file && file[0]) {
            switch_to_thread (any_thread_of_process (pid)->ptid);
            exec_file_attach (file, 0);
            symbol_file_add_main (file, 0);
        }
    }
    switch_to_thread (cur_ptid);

    /* This is a hack to populate the dwarf mapping tables now that we
       have the architecture at hand. Having these tables initialized
       from the debug reader routines will break as objfile_arch won't
       have the register descriptions */
    gdbarch_dwarf2_reg_to_regnum (get_current_arch(), 0);
}

void
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

void
_initialize__k1_target (void)
{
    simulation_vehicle = simulation_vehicles[0];
    
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

    k1_target_ops.to_magic = OPS_MAGIC;
    
    add_target (&k1_target_ops);

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

    add_com ("attach-mppa", class_run, attach_mppa_command, _("\
Connect to a MPPA TLM platform and start debugging it.\n\
Usage is `attach-mppa PORT'."));

    add_com ("run-mppa", class_run, run_mppa_command, _("\
Connect to a MPPA TLM platform and start debugging it."));


    observer_attach_inferior_created (k1_push_arch_stratum);
    k1_attached_inf_data = register_inferior_data_with_cleanup (NULL, mppa_inferior_data_cleanup);
}

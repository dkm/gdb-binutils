#ifndef _K1_TARGET_H_
#define _K1_TARGET_H_

enum en_debug_level {DBG_LEVEL_SYSTEM = 0, DBG_LEVEL_KERNEL_USER, 
  DBG_LEVEL_USER, DBG_LEVEL_INHERITED, DBG_LEVEL_MAX};
  
struct inferior_data
{
  const char *cluster;
  int booted;
  int idx_cluster_debug_level;
  int cluster_debug_level_postponed;
};

extern int idx_global_debug_level;
extern int global_debug_level_set;
extern int inf_created_change_th;
extern int opt_hide_threads;

void _initialize__k1_target (void);
void send_cluster_debug_level (int level);
int get_debug_level (int pid);
int get_cluster_debug_level (int pid);
int kalray_hide_thread (struct thread_info *tp, ptid_t crt_ptid);
int get_thread_mode_used_for_ptid (ptid_t ptid);
int get_cpu_exec_level (void);
const char *get_str_debug_level (int level);
int get_os_supported_debug_levels (struct inferior *inf);
void set_cluster_debug_level_no_check (struct inferior *inf, int debug_level);
void apply_global_debug_level (int level);
void send_stop_at_main (int bstop);

struct inferior_data *mppa_inferior_data (struct inferior *inf);


void set_general_thread (struct ptid ptid);

#endif //_K1_TARGET_H_

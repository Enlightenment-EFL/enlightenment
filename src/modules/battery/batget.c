#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <Ecore.h>
#include <Ecore_Con.h>
#include <Ecore_File.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef HAVE_CFBASE_H
# include <CFBase.h>
# include <CFNumber.h>
# include <CFArray.h>
# include <CFDictionary.h>
# include <CFRunLoop.h>
# include <ps/IOPSKeys.h>
# include <ps/IOPowerSources.h>
#endif

/* supported battery system schemes - irrespective of OS */
#define CHECK_NONE                   0
#define CHECK_ACPI                   1
#define CHECK_APM                    2
#define CHECK_PMU                    3
#define CHECK_SYS_CLASS_POWER_SUPPLY 4

#define SYS_PWR

static void      init(void);
static Eina_Bool poll_cb(void *data);

static Ecore_Timer *timer = NULL;

static int mode = CHECK_NONE;

static int time_left = -2;
static int battery_full = -2;
static int have_battery = -2;
static int have_power = -2;

static const char *sys_power_dir = "/sys/class/power_supply";

/* UTILS */
static int
int_file_get(const char *file)
{
   int val = -1;
   FILE *f = fopen(file, "r");
   if (f)
     {
        char buf[256];
        char *str = fgets(buf, sizeof(buf), f);
        if (str) val = atoi(str);
        fclose(f);
     }
   return val;
}

static char *
str_file_get(const char *file)
{
   char *val = NULL;
   FILE *f = fopen(file, "r");
   if (f)
     {
        char buf[4096];
        char *str = fgets(buf, sizeof(buf), f);
        if (str)
          {
             size_t len = strlen(str);
             if ((len > 0) && (str[len - 1] == '\n'))
               {
                  len--;
                  str[len] = 0;
               }
             val = malloc(len + 1);
             if (val) memcpy(val, str, len + 1);
          }
        fclose(f);
     }
   return val;
}

static int
int_get(const char *buf)
{
   const char *p = strchr(buf, ':');
   if (!p) return 0;
   p++;
   while (*p == ' ')
     p++;
   return atoi(p);
}

static char *
str_get(const char *buf)
{
   const char *p = strchr(buf, ':');
   const char *q;
   char *ret;

   if (!p) return NULL;
   p++;
   while (*p == ' ')
     p++;

   q = p + strlen(p) - 1;
   while ((q > p) && ((*q == ' ') || (*q == '\n')))
     q--;

   if (q < p) return NULL;
   q++;
   ret = malloc(q - p + 1);
   if (!ret) return NULL;
   memcpy(ret, p, q - p);
   ret[q - p] = '\0';
   return ret;
}

static char *
file_str_entry_get(FILE *f,
                   const char *entry)
{
   char buf[4096];
   char *tmp;

   tmp = fgets(buf, sizeof(buf), f);
   if (!tmp)
     {
        EINA_LOG_ERR("unexpected end of file, expected: '%s'", entry);
        return NULL;
     }
   if (strcmp(tmp, entry) != 0)
     {
        EINA_LOG_ERR("unexpected file entry, expected: '%s'", entry);
        return NULL;
     }
   tmp = str_get(tmp);
   if (!tmp)
     {
        EINA_LOG_ERR("unexpected file entry, missing value for '%s'", entry);
        return NULL;
     }
   return tmp;
}

#if defined(HAVE_CFBASE_H) /* OS X */
/***---***/
static void darwin_init(void);
static void darwin_check(void);

static void
darwin_init(void)
{
   /* nothing to do */
}

static void
darwin_check(void)
{
   const void *values;
   int device_num, device_count;
   int currentval = 0, maxval = 0;
   CFTypeRef blob;
   CFArrayRef sources;
   CFDictionaryRef device_dict;

   time_left = -1;
   battery_full = -1;
   have_battery = 0;
   have_power = 0;

   /* Retrieve the power source data and the array of sources. */
   blob = IOPSCopyPowerSourcesInfo();
   sources = IOPSCopyPowerSourcesList(blob);
   device_count = CFArrayGetCount(sources);
   for (device_num = 0; device_num < device_count; device_num++)
     {
        CFTypeRef ps;

        /* Retrieve a dictionary of values for this device and the count of keys in the dictionary. */
        ps = CFArrayGetValueAtIndex(sources, device_num);
        device_dict = IOPSGetPowerSourceDescription(blob, ps);
        /* Retrieve the charging key and save the present charging value if one exists. */
        if (CFDictionaryGetValueIfPresent(device_dict,
                                          CFSTR(kIOPSIsChargingKey), &values))
          {
             have_battery = 1;
             if (CFBooleanGetValue(values) > 0) have_power = 1;
             break;
          }
     }

   if (!have_battery)
     {
        CFRelease(sources);
        CFRelease(blob);
        have_power = 1;
        return;
     }

   /* Retrieve the current capacity key. */
   values = CFDictionaryGetValue(device_dict, CFSTR(kIOPSCurrentCapacityKey));
   CFNumberGetValue(values, kCFNumberSInt32Type, &currentval);
   /* Retrieve the max capacity key. */
   values = CFDictionaryGetValue(device_dict, CFSTR(kIOPSMaxCapacityKey));
   CFNumberGetValue(values, kCFNumberSInt32Type, &maxval);
   /* Calculate the percentage charged. */
   battery_full = (currentval * 10000) / maxval;

   /* Retrieve the remaining battery power or time until charged in minutes. */
   if (!have_power)
     {
        values = CFDictionaryGetValue(device_dict, CFSTR(kIOPSTimeToEmptyKey));
        CFNumberGetValue(values, kCFNumberSInt32Type, &currentval);
        time_left = currentval * 60;
     }
   else
     {
        values = CFDictionaryGetValue(device_dict, CFSTR(kIOPSTimeToFullChargeKey));
        CFNumberGetValue(values, kCFNumberSInt32Type, &currentval);
        time_left = currentval * 60;
     }
   CFRelease(sources);
   CFRelease(blob);
}

#else

/***---***/
/* new linux power class api to get power info - brand new and this code
 * may have bugs, but it is a good attempt to get it right */
static void      linux_sys_class_power_supply_init(void);

typedef struct _Sys_Class_Power_Supply_Uevent Sys_Class_Power_Supply_Uevent;

#define BASIS_CHARGE  1
#define BASIS_ENERGY  2
#define BASIS_VOLTAGE 3

struct _Sys_Class_Power_Supply_Uevent
{
   char             *name;
   int               fd;
   Ecore_Fd_Handler *fd_handler;

   int               present;

   int               basis;
   int               basis_empty;
   int               basis_full;

   unsigned char     have_current_avg : 1;
   unsigned char     have_current_now : 1;
};

static Eina_List *events = NULL;

static void
linux_sys_class_power_supply_sysev_init(Sys_Class_Power_Supply_Uevent *sysev)
{
   char buf[4096];
   const char *dir = sys_power_dir;

   sysev->basis = 0;
   sysev->have_current_avg = 0;
   sysev->have_current_now = 0;

   snprintf(buf, sizeof(buf), "%s/%s/present", dir, sysev->name);
   sysev->present = int_file_get(buf);
   if (!sysev->present) return;

   snprintf(buf, sizeof(buf), "%s/%s/current_avg", dir, sysev->name);
   if (ecore_file_exists(buf)) sysev->have_current_avg = 1;
   snprintf(buf, sizeof(buf), "%s/%s/current_now", dir, sysev->name);
   if (ecore_file_exists(buf)) sysev->have_current_now = 1;

   snprintf(buf, sizeof(buf), "%s/%s/voltage_max", dir, sysev->name);
   if (ecore_file_exists(buf)) sysev->basis = BASIS_VOLTAGE;
   snprintf(buf, sizeof(buf), "%s/%s/voltage_max_design", dir, sysev->name);
   if (ecore_file_exists(buf)) sysev->basis = BASIS_VOLTAGE;

   snprintf(buf, sizeof(buf), "%s/%s/energy_full", dir, sysev->name);
   if (ecore_file_exists(buf)) sysev->basis = BASIS_ENERGY;
   snprintf(buf, sizeof(buf), "%s/%s/energy_full_design", dir, sysev->name);
   if (ecore_file_exists(buf)) sysev->basis = BASIS_ENERGY;

   snprintf(buf, sizeof(buf), "%s/%s/charge_full", dir, sysev->name);
   if (ecore_file_exists(buf)) sysev->basis = BASIS_CHARGE;
   snprintf(buf, sizeof(buf), "%s/%s/charge_full_design", dir, sysev->name);
   if (ecore_file_exists(buf)) sysev->basis = BASIS_CHARGE;

   if (sysev->basis == BASIS_CHARGE)
     {
        snprintf(buf, sizeof(buf), "%s/%s/charge_full", dir, sysev->name);
        sysev->basis_full = int_file_get(buf);
        snprintf(buf, sizeof(buf), "%s/%s/charge_empty", dir, sysev->name);
        sysev->basis_empty = int_file_get(buf);
        if (sysev->basis_full < 0)
          {
             snprintf(buf, sizeof(buf), "%s/%s/charge_full_design", dir, sysev->name);
             sysev->basis_full = int_file_get(buf);
          }
        if (sysev->basis_empty < 0)
          {
             snprintf(buf, sizeof(buf), "%s/%s/charge_empty_design", dir, sysev->name);
             sysev->basis_empty = int_file_get(buf);
          }
     }
   else if (sysev->basis == BASIS_ENERGY)
     {
        snprintf(buf, sizeof(buf), "%s/%s/energy_full", dir, sysev->name);
        sysev->basis_full = int_file_get(buf);
        snprintf(buf, sizeof(buf), "%s/%s/energy_empty", dir, sysev->name);
        sysev->basis_empty = int_file_get(buf);
        if (sysev->basis_full < 0)
          {
             snprintf(buf, sizeof(buf), "%s/%s/energy_full_design", dir, sysev->name);
             sysev->basis_full = int_file_get(buf);
          }
        if (sysev->basis_empty < 0)
          {
             snprintf(buf, sizeof(buf), "%s/%s/energy_empty_design", dir, sysev->name);
             sysev->basis_empty = int_file_get(buf);
          }
     }
   else if (sysev->basis == BASIS_VOLTAGE)
     {
        snprintf(buf, sizeof(buf), "%s/%s/voltage_max", dir, sysev->name);
        sysev->basis_full = int_file_get(buf);
        snprintf(buf, sizeof(buf), "%s/%s/voltage_min", dir, sysev->name);
        sysev->basis_empty = int_file_get(buf);
        if (sysev->basis_full < 0)
          {
             snprintf(buf, sizeof(buf), "%s/%s/voltage_max_design", dir, sysev->name);
             sysev->basis_full = int_file_get(buf);
          }
        if (sysev->basis_empty < 0)
          {
             snprintf(buf, sizeof(buf), "%s/%s/voltage_min_design", dir, sysev->name);
             sysev->basis_empty = int_file_get(buf);
          }
     }
}

static int
linux_sys_class_power_supply_is_battery(char *name)
{
   int fd;
   int ret = 0;
   char buf[256];
   const char *dir = sys_power_dir;

   snprintf(buf, sizeof(buf), "%s/%s/type", dir, name);
   fd = open(buf, O_RDONLY);
   if (fd < 0)
     {
        ret = 0;
        goto NO_OPEN;
     }
   else if (read(fd, buf, sizeof(buf)) < 1)
     ret = 0;
   else if (!strncmp(buf, "Battery", 7))
     ret = 1;

   close(fd);

NO_OPEN:
   return ret;
}

static void
linux_sys_class_power_supply_init(void)
{
   Eina_List *l;

   if (events)
     {
        Sys_Class_Power_Supply_Uevent *sysev;

        EINA_LIST_FOREACH(events, l, sysev)
          linux_sys_class_power_supply_sysev_init(sysev);
     }
   else
     {
        Eina_List *bats;
        char *name;

        bats = ecore_file_ls("/sys/class/power_supply/");
        if (bats)
          {
             events = NULL;

             EINA_LIST_FREE(bats, name)
               {
                  Sys_Class_Power_Supply_Uevent *sysev;

                  if (!(linux_sys_class_power_supply_is_battery(name)))
                    {
                       free(name);
                       continue;
                    }

                  sysev = (Sys_Class_Power_Supply_Uevent *)calloc(1, sizeof(Sys_Class_Power_Supply_Uevent));
                  sysev->name = name;
                  events = eina_list_append(events, sysev);
                  linux_sys_class_power_supply_sysev_init(sysev);
               }
          }
     }
}

static void
linux_sys_class_power_supply_check(void)
{
   Eina_List *l;
   char *name;
   char buf[4096];
   const char *dir = sys_power_dir;

   battery_full = -1;
   time_left = -1;
   have_battery = 0;
   have_power = 0;

   if (events)
     {
        Sys_Class_Power_Supply_Uevent *sysev;
        int total_pwr_now;
        int total_pwr_max;
        int nofull = 0;

        total_pwr_now = 0;
        total_pwr_max = 0;
        time_left = 0;
        EINA_LIST_FOREACH(events, l, sysev)
          {
             char *tmp;
             int present = 0;
             int charging = -1;
             int capacity = -1;
             int current = -1;
             int time_to_full = -1;
             int time_to_empty = -1;
             int full = -1;
             int pwr_now = -1;
             int pwr_empty = -1;
             int pwr_full = -1;
             int pwr = 0;

             name = sysev->name;

             /* fetch more generic info */
             // init
             present = sysev->present;
             if (!present) continue;

             snprintf(buf, sizeof(buf), "%s/%s/capacity", dir, name);
             capacity = int_file_get(buf);
             if (sysev->have_current_avg)
               {
                  snprintf(buf, sizeof(buf), "%s/%s/current_avg", dir, name);
                  current = int_file_get(buf);
               }
             else if (sysev->have_current_now)
               {
                  snprintf(buf, sizeof(buf), "%s/%s/current_now", dir, name);
                  current = int_file_get(buf);
               }

             /* FIXME: do we get a uevent on going from charging to full?
              * if so, move this to init */
             snprintf(buf, sizeof(buf), "%s/%s/status", dir, name);
             tmp = str_file_get(buf);
             if (tmp)
               {
                  full = 0;
                  if (!strncasecmp("discharging", tmp, 11)) charging = 0;
                  else if (!strncasecmp("unknown", tmp, 7))
                    charging = 0;
                  else if (!strncasecmp("not charging", tmp, 12))
                    charging = 0;
                  else if (!strncasecmp("charging", tmp, 8))
                    charging = 1;
                  else if (!strncasecmp("full", tmp, 4))
                    {
                       full = 1;
                       charging = 0;
                    }
                  free(tmp);
               }
             /* some batteries can/will/want to predict how long they will
              * last. if so - take what the battery says. too bad if it's
              * wrong. that's a buggy battery or driver */
             if (!full)
               {
                  nofull++;
                  if (charging)
                    {
                       snprintf(buf, sizeof(buf), "%s/%s/time_to_full_now", dir, name);
                       time_to_full = int_file_get(buf);
                    }
                  else
                    {
                       snprintf(buf, sizeof(buf), "%s/%s/time_to_empty_now", dir, name);
                       time_to_empty = int_file_get(buf);
                    }
               }

             /* now get charge, energy and voltage. take the one that provides
              * the best info (charge first, then energy, then voltage */
             if (sysev->basis == BASIS_CHARGE)
               snprintf(buf, sizeof(buf), "%s/%s/charge_now", dir, name);
             else if (sysev->basis == BASIS_ENERGY)
               snprintf(buf, sizeof(buf), "%s/%s/energy_now", dir, name);
             else if (sysev->basis == BASIS_VOLTAGE)
               snprintf(buf, sizeof(buf), "%s/%s/voltage_now", dir, name);
             pwr_now = int_file_get(buf);
             pwr_empty = sysev->basis_empty;
             pwr_full = sysev->basis_full;
             if ((sysev->basis == BASIS_VOLTAGE) &&
                 (capacity >= 0))
               {
                  /* if we use voltage as basis.. we're not very accurate
                   * so we should prefer capacity readings */
                  pwr_empty = -1;
                  pwr_full = -1;
                  pwr_now = -1;
               }

             if (pwr_empty < 0) pwr_empty = 0;

             if ((pwr_full > 0) && (pwr_full > pwr_empty))
               {
                  if (full) pwr_now = pwr_full;
                  else
                    {
                       if (pwr_now < 0)
                         pwr_now = (((long long)capacity * ((long long)pwr_full - (long long)pwr_empty)) / 10000) + pwr_empty;
                    }

                  if (sysev->present) have_battery = 1;
                  if (charging)
                    {
                       have_power = 1;
                       if (time_to_full >= 0)
                         {
                            if (time_to_full > time_left)
                              time_left = time_to_full;
                         }
                       else
                         {
                            if (current == 0) time_left = 0;
                            else if (current < 0)
                              time_left = -1;
                            else
                              {
                                 pwr = (((long long)pwr_full - (long long)pwr_now) * 3600) / -current;
                                 if (pwr > time_left) time_left = pwr;
                              }
                         }
                    }
                  else
                    {
                       have_power = 0;
                       if (time_to_empty >= 0) time_left += time_to_empty;
                       else
                         {
                            if (time_to_empty < 0)
                              {
                                 if (current > 0)
                                   {
                                      pwr = (((long long)pwr_now - (long long)pwr_empty) * 3600) / current;
                                      time_left += pwr;
                                   }
                              }
                         }
                    }
                  total_pwr_now += pwr_now - pwr_empty;
                  total_pwr_max += pwr_full - pwr_empty;
               }
             /* simple current battery fallback */
             else
               {
                  if (sysev->present) have_battery = 1;
                  if (charging) have_power = 1;
                  total_pwr_max = 10000;
                  total_pwr_now = capacity;
                  if (total_pwr_now < 10000) nofull = 1;
               }
          }
        if (total_pwr_max > 0)
          battery_full = ((long long)total_pwr_now * 10000) / total_pwr_max;
        if (nofull == 0)
          time_left = -1;
     }
}

/***---***/
/* "here and now" ACPI based power checking. is there for linux and most
 * modern laptops. as of linux 2.6.24 it is replaced with
 * linux_sys_class_power_supply_init/check() though as this is the new
 * power class api to poll for power stuff
 */
static Eina_Bool linux_acpi_cb_acpid_add(void *data,
                                         int type,
                                         void *event);
static Eina_Bool linux_acpi_cb_acpid_del(void *data,
                                         int type,
                                         void *event);
static Eina_Bool linux_acpi_cb_acpid_data(void *data,
                                          int type,
                                          void *event);
static void      linux_acpi_init(void);
static void      linux_acpi_check(void);

static int acpi_max_full = -1;
static int acpi_max_design = -1;
static Ecore_Con_Server *acpid = NULL;
static Ecore_Event_Handler *acpid_handler_add = NULL;
static Ecore_Event_Handler *acpid_handler_del = NULL;
static Ecore_Event_Handler *acpid_handler_data = NULL;
static Ecore_Timer *delay_check = NULL;
static int event_fd = -1;
static Ecore_Fd_Handler *event_fd_handler = NULL;

static Eina_Bool
linux_acpi_cb_delay_check(void *data EINA_UNUSED)
{
   linux_acpi_init();
   poll_cb(NULL);
   delay_check = NULL;
   return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool
linux_acpi_cb_acpid_add(void *data  EINA_UNUSED,
                        int type    EINA_UNUSED,
                        void *event EINA_UNUSED)
{
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
linux_acpi_cb_acpid_del(void *data  EINA_UNUSED,
                        int type    EINA_UNUSED,
                        void *event EINA_UNUSED)
{
   ecore_con_server_del(acpid);
   acpid = NULL;
   if (acpid_handler_add) ecore_event_handler_del(acpid_handler_add);
   acpid_handler_add = NULL;
   if (acpid_handler_del) ecore_event_handler_del(acpid_handler_del);
   acpid_handler_del = NULL;
   if (acpid_handler_data) ecore_event_handler_del(acpid_handler_data);
   acpid_handler_data = NULL;
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
linux_acpi_cb_acpid_data(void *data  EINA_UNUSED,
                         int type    EINA_UNUSED,
                         void *event EINA_UNUSED)
{
   if (delay_check) ecore_timer_del(delay_check);
   delay_check = ecore_timer_loop_add(0.2, linux_acpi_cb_delay_check, NULL);
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
linux_acpi_cb_event_fd_active(void *data        EINA_UNUSED,
                              Ecore_Fd_Handler *fd_handler)
{
   if (ecore_main_fd_handler_active_get(fd_handler, ECORE_FD_READ))
     {
        int lost = 0;
        for (;; )
          {
             char buf[1024];
             int num;

             if ((num = read(event_fd, buf, sizeof(buf))) < 1)
               {
                  lost = ((errno == EIO) ||
                          (errno == EBADF) ||
                          (errno == EPIPE) ||
                          (errno == EINVAL) ||
                          (errno == ENOSPC));
                  if (num == 0) break;
               }
          }
        if (lost)
          {
             ecore_main_fd_handler_del(event_fd_handler);
             event_fd_handler = NULL;
             close(event_fd);
             event_fd = -1;
          }
        else
          {
             if (delay_check) ecore_timer_del(delay_check);
             delay_check = ecore_timer_loop_add(0.2, linux_acpi_cb_delay_check, NULL);
          }
     }
   return ECORE_CALLBACK_RENEW;
}

static void
linux_acpi_init(void)
{
   Eina_Iterator *powers;
   Eina_Iterator *bats;

   bats = eina_file_direct_ls("/proc/acpi/battery");
   if (bats)
     {
        Eina_File_Direct_Info *info;
        FILE *f;
        char *tmp;
        char buf[(PATH_MAX * 2) + 128];

        have_power = 0;
        powers = eina_file_direct_ls("/proc/acpi/ac_adapter");
        if (powers)
          {
             EINA_ITERATOR_FOREACH(powers, info)
               {
                  if (info->name_length + sizeof("/state") >= sizeof(buf)) continue;
                  snprintf(buf, sizeof(buf), "%s/state", info->path);
                  f = fopen(buf, "r");
                  if (f)
                    {
                       /* state */
                       tmp = fgets(buf, sizeof(buf), f);
                       if (tmp) tmp = str_get(tmp);
                       if (tmp)
                         {
                            if (!strcmp(tmp, "on-line")) have_power = 1;
                            free(tmp);
                         }
                       fclose(f);
                    }
               }
             eina_iterator_free(powers);
          }

        have_battery = 0;
        acpi_max_full = 0;
        acpi_max_design = 0;
        EINA_ITERATOR_FOREACH(bats, info)
          {
             snprintf(buf, sizeof(buf), "%s/info", info->path);
             f = fopen(buf, "r");
             if (f)
               {
                  /* present */
                  tmp = fgets(buf, sizeof(buf), f);
                  if (tmp) tmp = str_get(tmp);
                  if (tmp)
                    {
                       if (!strcmp(tmp, "yes")) have_battery = 1;
                       free(tmp);
                    }
                  /* design cap */
                  tmp = fgets(buf, sizeof(buf), f);
                  if (tmp) tmp = str_get(tmp);
                  if (tmp)
                    {
                       if (strcmp(tmp, "unknown")) acpi_max_design += atoi(tmp);
                       free(tmp);
                    }
                  /* last full cap */
                  tmp = fgets(buf, sizeof(buf), f);
                  if (tmp) tmp = str_get(tmp);
                  if (tmp)
                    {
                       if (strcmp(tmp, "unknown")) acpi_max_full += atoi(tmp);
                       free(tmp);
                    }
                  fclose(f);
               }
          }

        eina_iterator_free(bats);
     }
   if (!acpid)
     {
        acpid = ecore_con_server_connect(ECORE_CON_LOCAL_SYSTEM,
                                         "/var/run/acpid.socket", -1, NULL);
        if (acpid)
          {
             acpid_handler_add = ecore_event_handler_add(ECORE_CON_EVENT_SERVER_ADD,
                                                         linux_acpi_cb_acpid_add, NULL);
             acpid_handler_del = ecore_event_handler_add(ECORE_CON_EVENT_SERVER_DEL,
                                                         linux_acpi_cb_acpid_del, NULL);
             acpid_handler_data = ecore_event_handler_add(ECORE_CON_EVENT_SERVER_DATA,
                                                          linux_acpi_cb_acpid_data, NULL);
          }
        else
          {
             if (event_fd < 0)
               {
                  event_fd = open("/proc/acpi/event", O_RDONLY);
                  if (event_fd >= 0)
                    event_fd_handler = ecore_main_fd_handler_add(event_fd,
                                                                 ECORE_FD_READ,
                                                                 linux_acpi_cb_event_fd_active,
                                                                 NULL,
                                                                 NULL, NULL);
               }
          }
     }
}

static void
linux_acpi_check(void)
{
   Eina_List *bats;

   battery_full = -1;
   time_left = -1;
   have_battery = 0;
   have_power = 0;

   bats = ecore_file_ls("/proc/acpi/battery");
   if (bats)
     {
        char *name;
        int rate = 0;
        int capacity = 0;

        EINA_LIST_FREE(bats, name)
          {
             char buf[4096];
             char *tmp;
             FILE *f;

             snprintf(buf, sizeof(buf), "/proc/acpi/battery/%s/state", name);
             free(name);
             f = fopen(buf, "r");
             if (!f) continue;

             tmp = file_str_entry_get(f, "present:");
             if (!tmp) goto fclose_and_continue;
             if (!strcasecmp(tmp, "yes")) have_battery = 1;
             free(tmp);

             tmp = file_str_entry_get(f, "capacity state:");
             if (!tmp) goto fclose_and_continue;
             free(tmp);

             tmp = file_str_entry_get(f, "charging state:");
             if (!tmp) goto fclose_and_continue;
             if ((have_power == 0) && (!strcasecmp(tmp, "charging")))
               have_power = 1;
             free(tmp);

             tmp = file_str_entry_get(f, "present rate:");
             if (!tmp) goto fclose_and_continue;
             if (strcasecmp(tmp, "unknown")) rate += atoi(tmp);
             free(tmp);

             tmp = file_str_entry_get(f, "remaining capacity:");
             if (!tmp) goto fclose_and_continue;
             if (strcasecmp(tmp, "unknown")) capacity += atoi(tmp);
             free(tmp);

fclose_and_continue:
             fclose(f);
          }

        if (acpi_max_full > 0)
          battery_full = 10000 * (long long)capacity / acpi_max_full;
        else if (acpi_max_design > 0)
          battery_full = 10000 * (long long)capacity / acpi_max_design;
        else
          battery_full = -1;
        if (rate <= 0) time_left = -1;
        else
          {
             if (have_power)
               time_left = (3600 * ((long long)acpi_max_full - (long long)capacity)) / rate;
             else
               time_left = (3600 * (long long)capacity) / rate;
          }
     }
}

/***---***/
/* old school apm support - very old laptops and some devices support this.
 * this is here for legacy support and i wouldn't suggest spending any
 * effort on it as it is complete below as best i know, but could have missed
 * one or 2 things, but not worth fixing */
static void linux_apm_init(void);
static void linux_apm_check(void);

static void
linux_apm_init(void)
{
   /* nothing to do */
}

static void
linux_apm_check(void)
{
   FILE *f;
   char s1[32], s2[32], s3[32], *endptr;
   int apm_flags, ac_stat, bat_stat, bat_flags, bat_val, time_val;

   battery_full = -1;
   time_left = -1;
   have_battery = 0;
   have_power = 0;

   f = fopen("/proc/apm", "r");
   if (!f) return;

   if (fscanf(f, "%*s %*s %x %x %x %x %31s %31s %31s",
              &apm_flags, &ac_stat, &bat_stat, &bat_flags, s1, s2, s3) != 7)
     {
        fclose(f);
        return;
     }
   fclose(f);

   bat_val = strtol(s1, &endptr, 10);
   if (*endptr != '%') return;

   if (!strcmp(s3, "sec")) time_val = atoi(s2);
   else if (!strcmp(s3, "min"))
     time_val = atoi(s2) * 60;
   else time_val = 0;

   if ((bat_flags != 0xff) && (bat_flags & 0x80))
     {
        have_battery = 0;
        have_power = 0;
        battery_full = 10000;
        time_left = 0;
        return;
     }

   if (bat_val >= 0)
     {
        have_battery = 1;
        have_power = ac_stat;
        battery_full = bat_val * 100;
        if (battery_full > 10000) battery_full = 10000;
        if (ac_stat == 1) time_left = -1;
        else time_left = time_val;
     }
   else
     {
        switch (bat_stat)
          {
           case 0: /* high */
             have_battery = 1;
             have_power = ac_stat;
             battery_full = 10000;
             time_left = -1;
             break;

           case 1: /* medium */
             have_battery = 1;
             have_power = ac_stat;
             battery_full = 5000;
             time_left = -1;
             break;

           case 2: /* low */
             have_battery = 1;
             have_power = ac_stat;
             battery_full = 2500;
             time_left = -1;
             break;

           case 3: /* charging */
             have_battery = 1;
             have_power = ac_stat;
             battery_full = 10000;
             time_left = -1;
             break;
          }
     }
}

/***---***/
/* for older mac powerbooks. legacy as well like linux_apm_init/check. leave
 * it alone unless you have to touch it */
static void linux_pmu_init(void);
static void linux_pmu_check(void);

static void
linux_pmu_init(void)
{
   /* nothing to do */
}

static void
linux_pmu_check(void)
{
   FILE *f;
   char buf[4096];
   Eina_List *bats;
   char *name;
   int ac = 0;
   int charge = 0;
   int max_charge = 0;
   int seconds = 0;
   int curcharge = 0;
   int curmax = 0;

   f = fopen("/proc/pmu/info", "r");
   if (f)
     {
        char *tmp;
        /* Skip driver */
        tmp = fgets(buf, sizeof(buf), f);
        if (!tmp)
          {
             EINA_LOG_ERR("no driver info in /proc/pmu/info");
             goto fclose_and_continue;
          }
        /* Skip firmware */
        tmp = fgets(buf, sizeof(buf), f);
        if (!tmp)
          {
             EINA_LOG_ERR("no firmware info in /proc/pmu/info");
             goto fclose_and_continue;
          }
        /* Read ac */
        tmp = fgets(buf, sizeof(buf), f);
        if (!tmp)
          {
             EINA_LOG_ERR("no AC info in /proc/pmu/info");
             goto fclose_and_continue;
          }
        ac = int_get(buf);
fclose_and_continue:
        fclose(f);
     }
   bats = ecore_file_ls("/proc/pmu");
   if (bats)
     {
        have_battery = 1;
        have_power = ac;
        EINA_LIST_FREE(bats, name)
          {
             if (strncmp(name, "battery", 7)) continue;
             snprintf(buf, sizeof(buf), "/proc/pmu/%s", name);
             f = fopen(buf, "r");
             if (f)
               {
                  int timeleft = 0;
                  int current = 0;

                  while (fgets(buf, sizeof (buf), f))
                    {
                       char *token;

                       if ((token = strtok(buf, ":")))
                         {
                            if (!strncmp("charge", token, 6))
                              charge = atoi(strtok(0, ": "));
                            else if (!strncmp("max_charge", token, 9))
                              max_charge = atoi(strtok(0, ": "));
                            else if (!strncmp("current", token, 7))
                              current = atoi(strtok(0, ": "));
                            else if (!strncmp("time rem", token, 8))
                              timeleft = atoi(strtok(0, ": "));
                            else
                              strtok(0, ": ");
                         }
                    }
                  curmax += max_charge;
                  curcharge += charge;
                  fclose(f);
                  if (!current)
                    {
                       /* Neither charging nor discharging */
                    }
                  else if (!ac)
                    {
                       /* When on dc, we are discharging */
                       seconds += timeleft;
                    }
                  else
                    {
                       /* Charging - works in parallel */
                       seconds = MAX(timeleft, seconds);
                    }
               }

             free(name);
          }
        if (max_charge > 0) battery_full = ((long long)charge * 10000) / max_charge;
        else battery_full = 0;
        time_left = seconds;
     }
   else
     {
        have_power = ac;
        have_battery = 0;
        battery_full = -1;
        time_left = -1;
     }
}

#endif

static int
dir_has_contents(const char *dir)
{
   Eina_List *bats;
   char *file;
   int count;

   bats = ecore_file_ls(dir);

   count = eina_list_count(bats);
   EINA_LIST_FREE(bats, file)
     free(file);
   if (count > 0) return 1;
   return 0;
}

static void
init(void)
{
#if defined(HAVE_CFBASE_H) /* OS X */
   darwin_init();
#else
   if ((ecore_file_is_dir(sys_power_dir)) && (dir_has_contents(sys_power_dir)))
     {
        mode = CHECK_SYS_CLASS_POWER_SUPPLY;
        linux_sys_class_power_supply_init();
     }
   else if (ecore_file_is_dir("/proc/acpi")) /* <= 2.6.24 */
     {
        mode = CHECK_ACPI;
        linux_acpi_init();
     }
   else if (ecore_file_exists("/proc/apm"))
     {
        mode = CHECK_APM;
        linux_apm_init();
     }
   else if (ecore_file_is_dir("/proc/pmu"))
     {
        mode = CHECK_PMU;
        linux_pmu_init();
     }
#endif
}

static Eina_Bool
poll_cb(void *data EINA_UNUSED)
{
   int ptime_left;
   int pbattery_full;
   int phave_battery;
   int phave_power;

   ptime_left = time_left;
   pbattery_full = battery_full;
   phave_battery = have_battery;
   phave_power = have_power;

#if defined(HAVE_CFBASE_H) /* OS X */
   darwin_check();
   return ECORE_CALLBACK_RENEW;
#else
   switch (mode)
     {
      case CHECK_ACPI:
        linux_acpi_check();
        break;

      case CHECK_APM:
        linux_apm_check();
        break;

      case CHECK_PMU:
        linux_pmu_check();
        break;

      case CHECK_SYS_CLASS_POWER_SUPPLY:
        linux_sys_class_power_supply_check();
        break;

      default:
        battery_full = -1;
        time_left = -1;
        have_battery = 0;
        have_power = 0;
        break;
     }
#endif
   if ((ptime_left != time_left) ||
       (pbattery_full != battery_full) ||
       (phave_battery != have_battery) ||
       (phave_power != have_power))
     {
        if ((time_left < 0) &&
            ((have_battery) && (battery_full < 0)))
          printf("ERROR\n");
        else
          printf("%i %i %i %i %i\n",
                 battery_full, time_left, time_left, have_battery, have_power);
        fflush(stdout);
     }
   return ECORE_CALLBACK_RENEW;
}

int
main(int argc EINA_UNUSED, char **argv EINA_UNUSED)
{
  // argv[1] used to be poll_interval
   ecore_init();
   ecore_file_init();
   ecore_con_init();

   init();
   timer = ecore_timer_add(10.0, poll_cb, NULL);
   poll_cb(NULL);

   ecore_main_loop_begin();

   ecore_con_shutdown();
   ecore_file_shutdown();
   ecore_shutdown();

   return 0;
}


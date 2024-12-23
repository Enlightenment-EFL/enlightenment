#include "e.h"
#include <Ecore_X.h>


#define RANDR_VERSION_1_1 ((1 << 16) | 1)
#define RANDR_VERSION_1_2 ((1 << 16) | 2)
#define RANDR_VERSION_1_3 ((1 << 16) | 3)
#define RANDR_VERSION_1_4 ((1 << 16) | 4)

static char *_output_screen_get(Ecore_X_Window root, Ecore_X_Randr_Output o);
static Ecore_X_Randr_Edid_Display_Interface_Type _output_conn_type_get(Ecore_X_Window root, Ecore_X_Randr_Output o);
static char *_output_name_get(Ecore_X_Window root, Ecore_X_Randr_Output o);
static Eina_Bool _is_lid_name(const char *name);
static Eina_Bool _cb_screen_change(void *data, int type, void *event);
static Eina_Bool _cb_crtc_change(void *data, int type, void *event);
static Eina_Bool _cb_output_change(void *data, int type, void *event);
static Eina_Bool _output_name_find(Ecore_X_Window root, const char *name, Ecore_X_Randr_Output *outputs, int outputs_num, Ecore_X_Randr_Output *out_ret);
static Eina_Bool _output_exists(Ecore_X_Randr_Output out, Ecore_X_Randr_Crtc_Info *info);
static Eina_Bool _rotation_exists(int rot, Ecore_X_Randr_Crtc_Info *info);
static Ecore_X_Randr_Mode _mode_screen_find(Ecore_X_Window root, E_Randr2_Screen *s, Ecore_X_Randr_Output out);

static Eina_List *handlers;
static int use_xrandr = -1;

E_Comp_Screen_Iface xiface =
{
   .available = e_comp_x_randr_available,
   .init = e_comp_x_randr_init,
   .shutdown = e_comp_x_randr_shutdown,
   .create = e_comp_x_randr_create,
   .apply = e_comp_x_randr_config_apply,
   .relative_motion = EINA_FALSE,
   .backlight_enabled = EINA_TRUE,
};

static void
_e_comp_x_randr_pre_swap(void *data EINA_UNUSED, Evas *e EINA_UNUSED)
{
   if (!e_comp_config_get()->grab) return;
   if (!e_comp->grabbed) return;
   ecore_x_ungrab();
   e_comp->grabbed = 0;
}

static void
_e_comp_x_randr_grab_cb(void)
{
   if (!e_comp->grabbed)
     {
        ecore_x_grab();
        ecore_x_sync();
     }
   else
     ecore_x_ungrab();
}

static char *
_output_screen_get(Ecore_X_Window root, Ecore_X_Randr_Output o)
{
   // get the name of the screen - likely a model name or manufacturer name
   char *name;
   unsigned long len = 0;
   unsigned char *edid = ecore_x_randr_output_edid_get(root, o, &len);
   if (!edid) return NULL;
   name = ecore_x_randr_edid_display_name_get(edid, len);
   free(edid);
   if (name)
     {
        char *name2 = strdup(name);
        free(name);
        return name2;
     }
   return NULL;
}

static Ecore_X_Randr_Edid_Display_Interface_Type
_output_conn_type_get(Ecore_X_Window root, Ecore_X_Randr_Output o)
{
   // get what kind of connector (hdmi, dvi, displayport etc.) - vga is
   Ecore_X_Randr_Edid_Display_Interface_Type type;
   unsigned long len = 0;
   unsigned char *edid = ecore_x_randr_output_edid_get(root, o, &len);
   if (!edid) return ECORE_X_RANDR_EDID_DISPLAY_INTERFACE_UNDEFINED;
   type = ecore_x_randr_edid_display_interface_type_get(edid, len);
   free(edid);
   return type;
}

static char *
_output_name_get(Ecore_X_Window root, Ecore_X_Randr_Output o)
{
   // get the output name - like connector (hdmi-0, dp1, dvi-0-1 etc.)
   char *name = ecore_x_randr_output_name_get(root, o, NULL);
   if (name)
     {
        char *name2 = strdup(name);
        free(name);
        return name2;
     }
   return _output_screen_get(root, o);
}

static Eina_Bool
_is_lid_name(const char *name)
{
   // a fixed list of possible built in connector names - likely a laptop
   // or device internal display
   if (!name) return EINA_FALSE;
   if      (strstr(name, "LVDS")) return EINA_TRUE;
   else if (strstr(name, "lvds")) return EINA_TRUE;
   else if (strstr(name, "Lvds")) return EINA_TRUE;
   else if (strstr(name, "LCD"))  return EINA_TRUE;
   else if (strstr(name, "eDP"))  return EINA_TRUE;
   else if (strstr(name, "edp"))  return EINA_TRUE;
   else if (strstr(name, "EDP"))  return EINA_TRUE;
   else if (strstr(name, "DSI"))  return EINA_TRUE;
   else if (strstr(name, "dsi"))  return EINA_TRUE;
   return EINA_FALSE;
}

static char *
_edid_string_get(Ecore_X_Window root, Ecore_X_Randr_Output o)
{
   // convert the edid binary data into a hex string so we can use it as
   // part of a name
   unsigned char *edid = NULL;
   unsigned long len = 0;
   char *edid_str = NULL;

   edid = ecore_x_randr_output_edid_get(root, o, &len);
   if (edid)
     {
        unsigned int k, kk;

        edid_str = malloc((len * 2) + 1);
        if (edid_str)
          {
             const char *hexch = "0123456789abcdef";

             for (kk = 0, k = 0; k < len; k++)
               {
                  edid_str[kk    ] = hexch[(edid[k] >> 4) & 0xf];
                  edid_str[kk + 1] = hexch[ edid[k]       & 0xf];
                  kk += 2;
               }
             edid_str[kk] = 0;
          }
        free(edid);
     }
   return edid_str;
}

static E_Randr2_Screen *
_info_unconf_primary_find(E_Randr2 *r)
{
   Eina_List *l;
   E_Randr2_Screen *s, *s_primary = NULL;
   int priority = 0;

   EINA_LIST_FOREACH(r->screens, l, s)
     {
        if (!((s->config.enabled) &&
              (s->config.mode.w > 0) && (s->config.mode.h > 0) &&
              (s->config.geom.w > 0) && (s->config.geom.h > 0)))
          continue;
        if (s->config.priority > priority)
          {
             s_primary = s;
             priority = s->config.priority;
          }
     }
   return s_primary;
}

static E_Randr2_Screen *
_info_unconf_left_find(E_Randr2 *r)
{
   Eina_List *l;
   E_Randr2_Screen *s, *s_left = NULL;
   int left_x = 0x7fffffff;
   int left_size = 0;

   EINA_LIST_FOREACH(r->screens, l, s)
     {
        if (!((s->config.enabled) &&
              (s->config.mode.w > 0) && (s->config.mode.h > 0) &&
              (s->config.geom.w > 0) && (s->config.geom.h > 0)))
          continue;
        if ((s->config.geom.x <= left_x) &&
            ((s->config.geom.w * s->config.geom.h) > left_size))
          {
             left_size = s->config.geom.w * s->config.geom.h;
             left_x = s->config.geom.x;
             s_left = s;
          }
     }
   return s_left;
}

static E_Randr2_Screen *
_info_unconf_closest_find(E_Randr2 *r, E_Randr2_Screen *s2, Eina_Bool configured)
{
   Eina_List *l;
   E_Randr2_Screen *s, *s_sel = NULL;
   int dist = 0x7fffffff;
   int dx, dy;

   EINA_LIST_FOREACH(r->screens, l, s)
     {
        if (s == s2) continue;
        if (!((s->config.enabled) &&
              (s->config.mode.w > 0) && (s->config.mode.h > 0) &&
              (s->config.geom.w > 0) && (s->config.geom.h > 0)))
          continue;
        if ((!configured) &&
            (s->config.relative.mode != E_RANDR2_RELATIVE_UNKNOWN))
          continue;
        else if ((configured) &&
                 (s->config.relative.mode == E_RANDR2_RELATIVE_UNKNOWN))
          continue;
        dx = (s->config.geom.x + (s->config.geom.w / 2)) -
          (s2->config.geom.x + (s2->config.geom.w / 2));
        dy = (s->config.geom.y + (s->config.geom.h / 2)) -
          (s2->config.geom.y + (s2->config.geom.h / 2));
        dx = sqrt((dx * dx) + (dy * dy));
        if (dx < dist)
          {
             s_sel = s;
             dist = dx;
          }
     }
   return s_sel;
}

static void
_info_relative_fixup(E_Randr2 *r)
{
   E_Randr2_Screen *s, *s2;
   int d, dx, dy;

   s = _info_unconf_primary_find(r);
   if (s) s->config.relative.mode = E_RANDR2_RELATIVE_NONE;
   else
     {
        s = _info_unconf_left_find(r);
        if (!s) return;
        s->config.relative.mode = E_RANDR2_RELATIVE_NONE;
     }
   for (;;)
     {
        // find the next screen that is closest to the last one we configured
        /// that is still not configured yet
        s = _info_unconf_closest_find(r, s, EINA_FALSE);
        if (!s) break;
        s2 = _info_unconf_closest_find(r, s, EINA_TRUE);
        // fix up s->config.relative.mode, s->config.relative.to and
        // s->config.relative.align to match (as closely as possible)
        // the geometry given - config s relative to s2
        if (!s2) s->config.relative.mode = E_RANDR2_RELATIVE_NONE;
        else
          {
             s->config.relative.to = strdup(s2->id);
             s->config.relative.align = 0.0;
             s->config.relative.mode = E_RANDR2_RELATIVE_NONE;
             if ((s->config.geom.x + s->config.geom.w) <=
                 s2->config.geom.x)
               {
                  s->config.relative.mode = E_RANDR2_RELATIVE_TO_LEFT;
                  d = s->config.geom.h - s2->config.geom.h;
                  dy = s2->config.geom.y - s->config.geom.y;
                  if (d != 0)
                    s->config.relative.align = ((double)dy) / ((double)d);
               }
             else if (s->config.geom.x >=
                      (s2->config.geom.x + s2->config.geom.w))
               {
                  s->config.relative.mode = E_RANDR2_RELATIVE_TO_RIGHT;
                  d = s->config.geom.h - s2->config.geom.h;
                  dy = s2->config.geom.y - s->config.geom.y;
                  if (d != 0)
                    s->config.relative.align = ((double)dy) / ((double)d);
               }
             else if ((s->config.geom.y + s->config.geom.h) <=
                      s2->config.geom.y)
               {
                  s->config.relative.mode = E_RANDR2_RELATIVE_TO_ABOVE;
                  d = s->config.geom.w - s2->config.geom.w;
                  dx = s2->config.geom.x - s->config.geom.x;
                  if (d != 0)
                    s->config.relative.align = ((double)dx) / ((double)d);
               }
             else if (s->config.geom.y >=
                      (s2->config.geom.y + s2->config.geom.h))
               {
                  s->config.relative.mode = E_RANDR2_RELATIVE_TO_BELOW;
                  d = s->config.geom.w - s2->config.geom.w;
                  dx = s2->config.geom.x - s->config.geom.x;
                  if (d != 0)
                    s->config.relative.align = ((double)dx) / ((double)d);
               }
             else if ((s->config.geom.x == s2->config.geom.x) &&
                      (s->config.geom.y == s2->config.geom.y) &&
                      (s->config.geom.w == s2->config.geom.w) &&
                      (s->config.geom.h == s2->config.geom.h))
               {
                  s->config.relative.mode = E_RANDR2_RELATIVE_CLONE;
               }
             if (s->config.relative.align < 0.0)
               s->config.relative.align = 0.0;
             else if (s->config.relative.align > 1.0)
               s->config.relative.align = 1.0;
          }
     }
}

static Eina_Bool
_cb_screen_change(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_X_Event_Screen_Change *ev = event;
   printf("RRR: CB screen change...\n");
   ecore_x_randr_config_timestamp_get(ev->root);
   ecore_x_randr_screen_current_size_get(ev->root, NULL, NULL, NULL, NULL);
   ecore_x_sync();
   if (!e_randr2_cfg->ignore_hotplug_events)
     e_randr2_screen_refresh_queue(EINA_TRUE);
   return EINA_TRUE;
}

static Eina_Bool
_cb_crtc_change(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_X_Event_Randr_Crtc_Change *ev = event;
   printf("RRR: CB crtc change...\n");
   ecore_x_randr_config_timestamp_get(ev->win);
   ecore_x_randr_screen_current_size_get(ev->win, NULL, NULL, NULL, NULL);
   ecore_x_sync();
   if (!e_randr2_cfg->ignore_hotplug_events)
     e_randr2_screen_refresh_queue(EINA_FALSE);
   return EINA_TRUE;
}

static Eina_Bool
_cb_output_change(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_X_Event_Randr_Output_Change *ev = event;
   printf("RRR: CB output change...\n");
   ecore_x_randr_config_timestamp_get(ev->win);
   ecore_x_randr_screen_current_size_get(ev->win, NULL, NULL, NULL, NULL);
   ecore_x_sync();
   if (!e_randr2_cfg->ignore_hotplug_events)
     e_randr2_screen_refresh_queue(EINA_TRUE);
   return EINA_TRUE;
}

static Eina_Bool
_output_name_find(Ecore_X_Window root, const char *name, Ecore_X_Randr_Output *outputs, int outputs_num, Ecore_X_Randr_Output *out_ret)
{
   int i;
   char *n;

   for (i = 0; i < outputs_num; i++)
     {
        n = _output_name_get(root, outputs[i]);
        if ((n) && (!strcmp(n, name)))
          {
             free(n);
             *out_ret = outputs[i];
             return EINA_TRUE;
          }
        free(n);
     }
   return EINA_FALSE;
}

static Eina_Bool
_output_exists(Ecore_X_Randr_Output out, Ecore_X_Randr_Crtc_Info *info)
{
   int i;

   for (i = 0; i < info->npossible; i++)
     {
        if (out == info->possible[i]) return EINA_TRUE;
     }
   return EINA_FALSE;
}

static Eina_Bool
_rotation_exists(int rot, Ecore_X_Randr_Crtc_Info *info)
{
   if ((rot == 0) && (info->rotations & ECORE_X_RANDR_ORIENTATION_ROT_0))
     return EINA_TRUE;
   if ((rot == 90) && (info->rotations & ECORE_X_RANDR_ORIENTATION_ROT_90))
     return EINA_TRUE;
   if ((rot == 180) && (info->rotations & ECORE_X_RANDR_ORIENTATION_ROT_180))
     return EINA_TRUE;
   if ((rot == 270) && (info->rotations & ECORE_X_RANDR_ORIENTATION_ROT_270))
     return EINA_TRUE;
   return EINA_FALSE;
}

static Ecore_X_Randr_Mode
_mode_screen_find(Ecore_X_Window root, E_Randr2_Screen *s, Ecore_X_Randr_Output out)
{
   Ecore_X_Randr_Mode_Info *minfo;
   Ecore_X_Randr_Mode mode = 0, *modes;
   int modes_num = 0, modes_pref = 0, distance = 0x7fffffff;
   int diff, i;
   double refresh;

   modes = ecore_x_randr_output_modes_get(root, out, &modes_num, &modes_pref);
   if (!modes) printf("RRR: modes for '%s' FETCH FAILED!!!\n", s->info.name);
   printf("RRR: modes for '%s' are %p [%i]\n", s->info.name, modes, modes_num);
   if (modes)
     {
        for (i = 0; i < modes_num; i++)
          {
             minfo = ecore_x_randr_mode_info_get(root, modes[i]);
             if (!minfo) continue;
             refresh = (double)minfo->dotClock /
               (double)(minfo->hTotal * minfo->vTotal);
             diff =
               (100 * abs(s->config.mode.w - (int)minfo->width)) +
               (100 * abs(s->config.mode.h - (int)minfo->height)) +
               fabs((100 * s->config.mode.refresh) - (100 * refresh));
             if (diff < distance)
               {
                  mode = modes[i];
                  distance = diff;
               }
             ecore_x_randr_mode_info_free(minfo);
          }
        free(modes);
     }
   return mode;
}

static E_Dialog *screen_error_dialog = NULL;

static void
_screen_error_remove_cb(void *data EINA_UNUSED, E_Dialog *dia EINA_UNUSED)
{
   screen_error_dialog = NULL;
   e_object_del(E_OBJECT(dia));
}

static Eina_Bool
_cb_no_outputs_timer(void *data EINA_UNUSED)
{
   if (!screen_error_dialog)
     {
        screen_error_dialog = e_dialog_new(NULL, "E", "_error_dialog");

        e_dialog_title_set(screen_error_dialog, _("Screen setup Error"));
        e_dialog_text_set(screen_error_dialog, _("You seem to have no screens configured to<br>"
                                 "be on given the outputs you have. This should<br>"
                                 "be fixed by going to:<br>"
                                 "<br>"
                                 "<b>Settings -> Screen -> Screen Setup</><br>"
                                 "<br>"
                                 "And configure at least one screen to be on."));
       e_dialog_icon_set(screen_error_dialog, "dialog-error", 64);
       e_dialog_button_add(screen_error_dialog, _("OK"), NULL, _screen_error_remove_cb, NULL);
       e_dialog_button_focus_num(screen_error_dialog, 0);
       elm_win_center(screen_error_dialog->win, 1, 1);
       e_dialog_show(screen_error_dialog);
     }
   else
     {
       elm_win_raise(screen_error_dialog->win);
     }

   return EINA_FALSE;
}

E_API void
e_comp_x_randr_init(void)
{
   // add handler for randr screen change events
   E_LIST_HANDLER_APPEND(handlers, ECORE_X_EVENT_SCREEN_CHANGE,
                         _cb_screen_change, NULL);
   E_LIST_HANDLER_APPEND(handlers, ECORE_X_EVENT_RANDR_CRTC_CHANGE,
                         _cb_crtc_change, NULL);
   E_LIST_HANDLER_APPEND(handlers, ECORE_X_EVENT_RANDR_OUTPUT_CHANGE,
                         _cb_output_change, NULL);
   // if it's 1.2 or better then we can select for these events
   if (ecore_x_randr_version_get() >= RANDR_VERSION_1_2)
     {
        Ecore_X_Window root = ecore_x_window_root_first_get();
        ecore_x_randr_events_select(root, EINA_TRUE);
     }
}

E_API void
e_comp_x_randr_shutdown(void)
{
   // clear up event listening
   if (ecore_x_randr_version_get() >= RANDR_VERSION_1_2)
     {
        Ecore_X_Window root = ecore_x_window_root_first_get();
        ecore_x_randr_events_select(root, EINA_FALSE);
     }
   E_FREE_LIST(handlers, ecore_event_handler_del);
}

static void
_remove_unused_crtcs(Ecore_X_Randr_Crtc crtc, Ecore_X_Randr_Crtc *unused_crtcs, int *unused_crtcs_num)
{ // remove from array of unused crtcs if in it
  int i;

  for (i = 0; i < (*unused_crtcs_num); i++)
    { // walk unused crtcs
      if (unused_crtcs[i] == crtc)
        { // if found shuffle down all above this slot + unused--
          for (;i < ((*unused_crtcs_num) - 1); i++)
            unused_crtcs[i] = unused_crtcs[i + 1];
          (*unused_crtcs_num)--;
          return;
        }
    }
}

static void
_e_comp_xrandr_cmd(void)
{
   Eina_List *l;
   E_Randr2_Screen *s;
   Ecore_X_Window root = ecore_x_window_root_first_get();
   int top_priority = 0;
   Ecore_X_Randr_Crtc *crtcs = NULL;
   Ecore_X_Randr_Crtc *unused_crtcs = NULL;
   Ecore_X_Randr_Output *outputs = NULL, out, *outconf;
   E_Randr2_Screen **screenconf;
   Ecore_X_Randr_Crtc_Info *info;
   int crtcs_num = 0, outputs_num = 0, i, j, numout, unused_crtcs_num = 0;

   crtcs = ecore_x_randr_crtcs_get(root, &crtcs_num);
   outputs = ecore_x_randr_outputs_get(root, &outputs_num);

   printf("RRR: crtcs=%p outputs=%p\n", crtcs, outputs);
   if ((crtcs) && (outputs))
     {
        outconf = alloca(outputs_num * sizeof(Ecore_X_Randr_Output));
        screenconf = alloca(outputs_num * sizeof(E_Randr2_Screen *));
        memset(outconf, 0, outputs_num * sizeof(Ecore_X_Randr_Output));
        memset(screenconf, 0, outputs_num * sizeof(E_Randr2_Screen *));

        printf("RRR: crtcs: ");
        for (i = 0; i < crtcs_num; i++)
          printf(" %i", crtcs[i]);
        printf("\n");
        unused_crtcs = alloca(crtcs_num * sizeof(Ecore_X_Randr_Crtc));
        for (i = 0; i < crtcs_num; i++) unused_crtcs[i] = crtcs[i];
        unused_crtcs_num = crtcs_num;
        for (i = 0; i < outputs_num; i++)
         {
           Ecore_X_Randr_Crtc crtc = ecore_x_randr_output_crtc_get(root,
                                                                   outputs[i]);
           if (crtc)
             _remove_unused_crtcs(crtc, unused_crtcs, &unused_crtcs_num);
         }
        // decide which outputs get which crtcs
        EINA_LIST_FOREACH(e_randr2->screens, l, s)
          {
             printf("RRR: find output for '%s'\n", s->info.name);
             // XXX: find clones and set them as outputs in an array
             if ((s->config.configured) &&
                 (_output_name_find(root, s->info.name, outputs,
                                    outputs_num, &out)))
               {
                  printf("RRR:   enabled: %i\n", s->config.enabled);
                  if (s->config.enabled)
                    {
                       if (s->config.priority > top_priority)
                         top_priority = s->config.priority;
                       for (i = 0; i < outputs_num; i++)
                         {
                            if (!screenconf[i])
                              {
                                 Ecore_X_Randr_Crtc crtc =
                                   ecore_x_randr_output_crtc_get(root,
                                                                 outputs[i]);
                                 info = NULL;
                                 printf("RRR:     crtc slot empty: %i, crtc=%i, output=%i\n", i, crtc, outputs[i]);
                                 if (crtc)
                                   info = ecore_x_randr_crtc_info_get(root,
                                                                      crtc);
                                else
                                  {
                                    int poss_crtcs_num = 0;
                                    Ecore_X_Randr_Crtc *poss_crtcs = ecore_x_randr_output_possible_crtcs_get(root, outputs[i], &crtcs_num);
                                    if (!poss_crtcs) printf("RRR:     no possible crtcs!!!\n");
                                    else
                                      {
                                        printf("RRR:     possible crtc: ");
                                        for (j = 0; j < poss_crtcs_num; j++)
                                          printf(" %i", poss_crtcs[j]);
                                        printf("\n");
                                        free(poss_crtcs);
                                      }
                                    if (unused_crtcs_num > 0)
                                      {
                                        Ecore_X_Randr_Crtc new_crtc = unused_crtcs[0];
                                        _remove_unused_crtcs(new_crtc, unused_crtcs, &unused_crtcs_num);
                                        printf("RRR:     assign crtc %i to output\n", new_crtc);
                                        ecore_x_randr_output_crtc_set(root, outputs[i], new_crtc);
                                        info = ecore_x_randr_crtc_info_get(root, new_crtc);
                                      }
                                    else
                                      {
                                        printf("RRR:     no available crtcs for the output\n");
                                      }
                                  }
                                 printf("RRR:     crtc info = %p\n", info);
                                 if (info)
                                   {
                                      printf("RRR:     output exists=%i rot exists=%i\n",
                                             _output_exists(out, info), _rotation_exists(s->config.rotation, info));
                                      if (_output_exists(out, info) &&
                                          _rotation_exists(s->config.rotation,
                                                           info))
                                        {
                                           printf("RRR:       assign slot out: %x\n", out);
                                           outconf[i] = out;
                                           screenconf[i] = s;
                                        }
                                      ecore_x_randr_crtc_info_free(info);
                                   }
                                 if (!screenconf[i])
                                   {
                                      printf("RRR:       assign slot off\n");
                                      outconf[i] = 0;
                                      screenconf[i] = s;
                                   }
                                 break;
                              }
                         }
                    }
                  else
                    {
                      for (i = 0; i < outputs_num; i++)
                         {
                            if (!screenconf[i])
                              {
                                 printf("RRR:       assign slot off 2\n");
                                 outconf[i] = 0;
                                 screenconf[i] = s;
                                 break;
                              }
                         }
                    }
               }
          }
        numout = 0;
        for (i = 0; i < outputs_num; i++)
          {
             if (screenconf[i]) numout++;
          }
        if (numout)
          {
             Eina_Strbuf *sb;
             E_Randr2_Screen *sc;

             sb = eina_strbuf_new();
             eina_strbuf_append(sb, "xrandr ");
             // set up a crtc to drive each output (or not)
             for (i = 0; i < numout; i++)
               {
                  sc = screenconf[i];
                  if (!sc) continue;
                  if (!sc->info.name) continue;
                  eina_strbuf_append_printf(sb, "--output '%s' ",
                                            sc->info.name);
                  // XXX: find clones and set them as outputs in an array
                  if (outconf[i])
                    {
                       eina_strbuf_append_printf(sb, "--mode %ix%i ",
                                                 sc->config.mode.w,
                                                 sc->config.mode.h);
                       eina_strbuf_append_printf(sb, "--refresh %i.%02i ",
                                                 (int)sc->config.mode.refresh,
                                                 (int)((sc->config.mode.refresh - (int)sc->config.mode.refresh) * 100));
                       eina_strbuf_append_printf(sb, "--rotate ");
                       if (screenconf[i]->config.rotation == 0)
                         eina_strbuf_append_printf(sb, "normal ");
                       else if (screenconf[i]->config.rotation == 90)
                         eina_strbuf_append_printf(sb, "left ");
                       else if (screenconf[i]->config.rotation == 180)
                         eina_strbuf_append_printf(sb, "inverted ");
                       else if (screenconf[i]->config.rotation == 270)
                         eina_strbuf_append_printf(sb, "right ");
                       eina_strbuf_append_printf(sb, "--pos %ix%i ",
                                                 sc->config.geom.x,
                                                 sc->config.geom.y);
                       if (screenconf[i]->config.priority == top_priority)
                         {
                            eina_strbuf_append_printf(sb, "--primary ");
                            top_priority = -1;
                         }
                    }
                  else
                    {
                       printf("RRR: crtc off: %i\n", i);
                       eina_strbuf_append_printf(sb, "--off ");
                    }
               }
             printf("RRR: XRANDR: %s\n", eina_strbuf_string_get(sb));
             ecore_exe_run(eina_strbuf_string_get(sb), NULL);
             eina_strbuf_free(sb);
          }
        else
          {
             printf("RRR: EERRRRRROOOORRRRRRR no outputs to configure!\n");
             ecore_timer_add(5.0, _cb_no_outputs_timer, NULL);
             ecore_x_root_screen_barriers_set(NULL, 0);
          }
     }
   free(outputs);
   free(crtcs);
}

static void
_e_comp_xrandr_ecore_x(void)
{

   Eina_List *l;
   E_Randr2_Screen *s;
   Ecore_X_Window root = ecore_x_window_root_first_get();
   int minw, minh, maxw, maxh, nw, nh, pw, ph, ww, hh;
   Ecore_X_Randr_Crtc *crtcs = NULL;
   Ecore_X_Randr_Output *outputs = NULL, out, *outconf;
   E_Randr2_Screen **screenconf;
   int crtcs_num = 0, outputs_num = 0, i, numout;
   Ecore_X_Randr_Crtc_Info *info;
   int top_priority = 0;
   int px, py;
   Eina_Bool fix_pointer = EINA_TRUE;

   ecore_x_grab();
   // set virtual resolution
   nw = e_randr2->w;
   nh = e_randr2->h;
   ecore_x_randr_screen_size_range_get(root, &minw, &minh, &maxw, &maxh);
   ecore_x_randr_screen_current_size_get(root, &pw, &ph, NULL, NULL);
     {
        int dww = 0, dhh = 0, dww2 = 0, dhh2 = 0;
        ecore_x_randr_screen_current_size_get(root, &dww, &dhh, &dww2, &dhh2);
        printf("RRR: cur size: %ix%i\n", dww, dhh);
     }
   printf("RRR: size range: %ix%i -> %ix%i\n", minw, minh, maxw, maxh);
   if (nw > maxw) nw = maxw;
   if (nh > maxh) nh = maxh;
   if (nw < minw) nw = minw;
   if (nh < minh) nh = minh;
   ww = nw; if (nw < pw) ww = pw;
   hh = nh; if (nh < ph) hh = ph;

   ecore_x_pointer_xy_get(root, &px, &py);
   ecore_x_randr_screen_current_size_set(root, ww, hh, -1, -1);
     {
        int dww = 0, dhh = 0, dww2 = 0, dhh2 = 0;
        ecore_x_randr_screen_current_size_get(root, &dww, &dhh, &dww2, &dhh2);
        printf("RRR: cur size: %ix%i\n", dww, dhh);
     }
   printf("RRR: set vsize: %ix%i\n", nw, nh);

   crtcs = ecore_x_randr_crtcs_get(root, &crtcs_num);
   outputs = ecore_x_randr_outputs_get(root, &outputs_num);

   printf("RRR: crtcs=%p outputs=%p\n", crtcs, outputs);
   if ((crtcs) && (outputs))
     {
        outconf = alloca(outputs_num * sizeof(Ecore_X_Randr_Output));
        screenconf = alloca(outputs_num * sizeof(E_Randr2_Screen *));
        memset(outconf, 0, outputs_num * sizeof(Ecore_X_Randr_Output));
        memset(screenconf, 0, outputs_num * sizeof(E_Randr2_Screen *));

        // decide which outputs get which crtcs
        EINA_LIST_FOREACH(e_randr2->screens, l, s)
          {
             printf("RRR: find output for '%s'\n", s->info.name);
             // XXX: find clones and set them as outputs in an array
             if ((s->config.configured) &&
                 (_output_name_find(root, s->info.name, outputs,
                                    outputs_num, &out)))
               {
                  printf("RRR:   enabled: %i\n", s->config.enabled);
                  if (s->config.enabled)
                    {
                       if (s->config.priority > top_priority)
                         top_priority = s->config.priority;
                       for (i = 0; i < outputs_num; i++)
                         {
                            if (!outconf[i])
                              {
                                 printf("RRR:     crtc slot empty: %i\n", i);
                                 info = ecore_x_randr_crtc_info_get(root,
                                                                    crtcs[i]);
                                 if (info)
                                   {
                                      if (_output_exists(out, info) &&
                                          _rotation_exists(s->config.rotation,
                                                           info))
                                        {
                                           printf("RRR:       assign slot out: %x\n", out);
                                           outconf[i] = out;
                                           screenconf[i] = s;
                                           ecore_x_randr_crtc_info_free(info);
                                           break;
                                        }
                                      ecore_x_randr_crtc_info_free(info);
                                   }
                              }
                         }
                    }
               }
          }
        numout = 0;
        for (i = 0; i < outputs_num; i++)
          {
             if (outconf[i]) numout++;
          }
        if (numout)
          {
             Ecore_X_Rectangle *scrs = alloca(numout * sizeof(Ecore_X_Rectangle));
             int scrs_num;

             scrs_num = 0;
             // set up a crtc to drive each output (or not)
             for (i = 0; i < numout; i++)
               {
                  // XXX: find clones and set them as outputs in an array
                  if (outconf[i])
                    {
                       Ecore_X_Randr_Orientation orient =
                         ECORE_X_RANDR_ORIENTATION_ROT_0;
                       Ecore_X_Randr_Mode mode;

                       mode = _mode_screen_find(root, screenconf[i], outconf[i]);
                       if (screenconf[i]->config.rotation == 0)
                         orient = ECORE_X_RANDR_ORIENTATION_ROT_0;
                       else if (screenconf[i]->config.rotation == 90)
                         orient = ECORE_X_RANDR_ORIENTATION_ROT_90;
                       else if (screenconf[i]->config.rotation == 180)
                         orient = ECORE_X_RANDR_ORIENTATION_ROT_180;
                       else if (screenconf[i]->config.rotation == 270)
                         orient = ECORE_X_RANDR_ORIENTATION_ROT_270;
                       printf("RRR: crtc on: %i = '%s'     @ %i %i    - %ix%i orient %i mode %x out %x\n",
                              i, screenconf[i]->info.name,
                              screenconf[i]->config.geom.x,
                              screenconf[i]->config.geom.y,
                              screenconf[i]->config.geom.w,
                              screenconf[i]->config.geom.h,
                              orient, mode, outconf[i]);
                       if (!ecore_x_randr_crtc_settings_set
                             (root, crtcs[i], &(outconf[i]), 1,
                              screenconf[i]->config.geom.x,
                              screenconf[i]->config.geom.y,
                              mode, orient))
                         printf("RRR:   failed to set crtc!!!!!!\n");
                       if (E_INSIDE(px, py,
                                    screenconf[i]->config.geom.x,
                                    screenconf[i]->config.geom.y,
                                    screenconf[i]->config.geom.w,
                                    screenconf[i]->config.geom.h))
                         fix_pointer = EINA_FALSE;
                       px = screenconf[i]->config.geom.x +
                         (screenconf[i]->config.geom.w / 2);
                       py = screenconf[i]->config.geom.y +
                         (screenconf[i]->config.geom.h / 2);
                       ecore_x_randr_crtc_panning_area_set
                         (root, crtcs[i],
                          screenconf[i]->config.geom.x,
                          screenconf[i]->config.geom.y,
                          screenconf[i]->config.geom.w,
                          screenconf[i]->config.geom.h);
                       if (screenconf[i]->config.priority == top_priority)
                         {
                            ecore_x_randr_primary_output_set(root, outconf[i]);
                            top_priority = -1;
                         }
                       scrs[scrs_num].x      = screenconf[i]->config.geom.x;
                       scrs[scrs_num].y      = screenconf[i]->config.geom.y;
                       scrs[scrs_num].width  = screenconf[i]->config.geom.w;
                       scrs[scrs_num].height = screenconf[i]->config.geom.h;
                       scrs_num++;
                    }
                  else
                    {
                       printf("RRR: crtc off: %i\n", i);
                       ecore_x_randr_crtc_settings_set
                         (root, crtcs[i], NULL, 0, 0, 0, 0,
                          ECORE_X_RANDR_ORIENTATION_ROT_0);
                    }
               }
             ecore_x_root_screen_barriers_set(scrs, scrs_num);
          }
        else
          {
             printf("RRR: EERRRRRROOOORRRRRRR no outputs to configure!\n");
             ecore_timer_add(5.0, _cb_no_outputs_timer, NULL);
             ecore_x_root_screen_barriers_set(NULL, 0);
          }
     }
   if (fix_pointer)
     {
        ecore_x_pointer_warp(root, px, py);
     }
   free(outputs);
   free(crtcs);

   printf("RRR: set vsize2: %ix%i\n", nw, nh);
   ecore_x_randr_screen_current_size_set(root, nw, nh, -1, -1);
     {
        int dww = 0, dhh = 0, dww2 = 0, dhh2 = 0;
        ecore_x_randr_screen_current_size_get(root, &dww, &dhh, &dww2, &dhh2);
        printf("RRR: cur size: %ix%i\n", dww, dhh);
//        ecore_x_randr_screen_reset(root);
//        ecore_x_randr_screen_current_size_set(root, nw, nh, -1, -1);
//        ecore_x_sync();
//        ecore_x_randr_screen_current_size_get(root, &dww, &dhh, &dww2, &dhh2);
//        printf("RRR: cur size: %ix%i\n", dww,d hh);
     }
   ecore_x_randr_screen_size_range_get(root, NULL, NULL, NULL, NULL);
   ecore_x_ungrab();
   ecore_x_sync();
   // ignore the next batch of randr events - we caused them ourselves
   // XXX: a problem. thew first time we configure the screen we may not
   // get any events back to clear the ignore flag below, so only apply
   // here if the randr config now doesn't match what we want to set up.
//   event_ignore = EINA_TRUE;
}

E_API void
e_comp_x_randr_config_apply(void)
{
  if (e_randr2_cfg->use_cmd)
    {
      if (use_xrandr == -1)
        {
          if (ecore_file_app_installed("xrandr")) use_xrandr = 1;
          else use_xrandr = 0;
        }
    }
  else use_xrandr = 0;
  if (use_xrandr == 1) _e_comp_xrandr_cmd();
  else _e_comp_xrandr_ecore_x();
}

E_API Eina_Bool
e_comp_x_randr_available(void)
{
   // is randr extn there?
   return ecore_x_randr_query();
}

E_API E_Randr2 *
e_comp_x_randr_create(void)
{
   Ecore_X_Randr_Crtc *crtcs = NULL;
   Ecore_X_Randr_Output *outputs = NULL;
   E_Zone *zone;
   int crtcs_num = 0, outputs_num = 0, i, j, k;
   Ecore_X_Window root = ecore_x_window_root_first_get();
   Ecore_X_Atom atom;
   static Ecore_X_Atom atom_panel = 0;
   // panel atoms...
   // unknown VGA DVI DVI‐I DVI‐A DVI‐D HDMI Panel
   // TV TV-Composite TV-SVideo TV-Component
   // TV-SCART TV-C4 DisplayPort
   E_Randr2 *r = calloc(1, sizeof(E_Randr2));
   if (!r) return NULL;

   printf("RRR: ................. info get!\n");
   // do this to force xrandr to update its content
   ecore_x_randr_screen_refresh(root);
   ecore_x_randr_config_timestamp_get(root);
   ecore_x_randr_screen_size_range_get(root, NULL, NULL, NULL, NULL);
   ecore_x_sync();

   crtcs = ecore_x_randr_crtcs_get(root, &crtcs_num);
   outputs = ecore_x_randr_outputs_get(root, &outputs_num);

   if (atom_panel == 0) atom_panel = ecore_x_atom_get("Panel");

   for (i = 0; i < outputs_num; i++)
     {
        Ecore_X_Randr_Mode *modes;
        Ecore_X_Randr_Edid_Display_Interface_Type conn;
        int modes_num = 0, modes_pref = 0, priority;
        E_Config_Randr2_Screen *cs;
        E_Randr2_Screen *s = calloc(1, sizeof(E_Randr2_Screen));
        if (!s) continue;

        printf("RRR: NEW SCREEN ...\n");
        s->info.name = _output_name_get(root, outputs[i]);
        printf("RRR: .... out %s\n", s->info.name);
        if (!s->info.name)
          {
             free(s);
             continue;
          }
        s->info.screen = _output_screen_get(root, outputs[i]);
        s->info.edid = _edid_string_get(root, outputs[i]);
        if (s->info.edid)
        s->id = malloc(strlen(s->info.name) + 1 + strlen(s->info.edid) + 1);
        else
        s->id = malloc(strlen(s->info.name) + 1 + 1);
        if (!s->id)
          {
             free(s->info.screen);
             free(s->info.edid);
             free(s);
             continue;
          }
        strcpy(s->id, s->info.name);
        strcat(s->id, "/");
        if (s->info.edid) strcat(s->id, s->info.edid);
        conn = _output_conn_type_get(root, outputs[i]);
        if (conn == ECORE_X_RANDR_EDID_DISPLAY_INTERFACE_UNDEFINED)
          s->info.connector = E_RANDR2_CONNECTOR_UNDEFINED;
        else if (conn == ECORE_X_RANDR_EDID_DISPLAY_INTERFACE_DVI)
          s->info.connector = E_RANDR2_CONNECTOR_DVI;
        else if (conn == ECORE_X_RANDR_EDID_DISPLAY_INTERFACE_HDMI_A)
          s->info.connector = E_RANDR2_CONNECTOR_HDMI_A;
        else if (conn == ECORE_X_RANDR_EDID_DISPLAY_INTERFACE_HDMI_B)
          s->info.connector = E_RANDR2_CONNECTOR_HDMI_B;
        else if (conn == ECORE_X_RANDR_EDID_DISPLAY_INTERFACE_MDDI)
          s->info.connector = E_RANDR2_CONNECTOR_MDDI;
        else if (conn == ECORE_X_RANDR_EDID_DISPLAY_INTERFACE_DISPLAY_PORT)
          s->info.connector = E_RANDR2_CONNECTOR_DISPLAY_PORT;
        s->info.is_lid = _is_lid_name(s->info.name);
        s->info.lid_closed = s->info.is_lid && e_acpi_lid_is_closed();
        atom = ecore_x_randr_output_connector_type_get(root, outputs[i]);
        if (atom == atom_panel) s->info.is_lid = EINA_TRUE;
        printf("RRR: ...... lid_closed = %i (%i && %i)\n", s->info.lid_closed, s->info.is_lid, e_acpi_lid_is_closed());
        if (ecore_x_randr_output_connection_status_get(root, outputs[i]) ==
            ECORE_X_RANDR_CONNECTION_STATUS_CONNECTED)
          s->info.connected = EINA_TRUE;
        printf("RRR: ...... connected %i\n", s->info.connected);
        if (ecore_x_randr_output_backlight_level_get(root, outputs[i]) >= 0.0)
          s->info.backlight = EINA_TRUE;
        ecore_x_randr_output_size_mm_get(root, outputs[i],
                                         &(s->info.size.w), &(s->info.size.h));
        modes = ecore_x_randr_output_modes_get(root, outputs[i],
                                               &modes_num, &modes_pref);
        printf("RRR: ...... modes %p\n", modes);
        if (modes)
          {
             for (j = 0; j < modes_num; j++)
               {
                  Ecore_X_Randr_Mode_Info *minfo =
                    ecore_x_randr_mode_info_get(root, modes[j]);
                  if (minfo)
                    {
                       E_Randr2_Mode *m = calloc(1, sizeof(E_Randr2_Mode));
                       if (m)
                         {
                            m->w = minfo->width;
                            m->h = minfo->height;
                            m->refresh =
                            (double)minfo->dotClock /
                            (double)(minfo->hTotal * minfo->vTotal);
                            m->flags = minfo->modeFlags;
                            if (j == (modes_pref - 1))
                              m->preferred = EINA_TRUE;
                            s->info.modes = eina_list_append(s->info.modes, m);
                         }
                       ecore_x_randr_mode_info_free(minfo);
                    }
               }
             free(modes);
             e_randr2_screen_modes_sort(s);
          }
        cs = NULL;
        priority = 0;
        if (e_randr2_cfg) cs = e_randr2_config_screen_find(s, e_randr2_cfg);
        if (cs)
          priority = cs->priority;
        else if (ecore_x_randr_primary_output_get(root) == outputs[i])
          priority = 100;
        s->config.priority = priority;
        for (j = 0; j < crtcs_num; j++)
          {
             Eina_Bool ok, possible;
             Ecore_X_Randr_Crtc_Info *info =
               ecore_x_randr_crtc_info_get(root, crtcs[j]);
             if (info)
               {
                  ok = EINA_FALSE;
                  possible = EINA_FALSE;
                  for (k = 0; k < info->noutput; k++)
                    {
                       if (info->outputs[k] == outputs[i])
                         {
                            ok = EINA_TRUE;
                            break;
                         }
                    }
                  if (!ok)
                    {
                       for (k = 0; k < info->npossible; k++)
                         {
                            if (info->possible[k] == outputs[i])
                              {
                                 ok = EINA_TRUE;
                                 possible = EINA_TRUE;
                                 break;
                              }
                         }
                    }
                  if (ok)
                    {
                       if (!possible)
                         {
                            Ecore_X_Randr_Mode_Info *minfo;

                            s->config.geom.x = info->x;
                            s->config.geom.y = info->y;
                            s->config.geom.w = info->width;
                            s->config.geom.h = info->height;
                            if (info->rotation & ECORE_X_RANDR_ORIENTATION_ROT_0)
                              s->config.rotation = 0;
                            else if (info->rotation & ECORE_X_RANDR_ORIENTATION_ROT_90)
                              s->config.rotation = 90;
                            else if (info->rotation & ECORE_X_RANDR_ORIENTATION_ROT_180)
                              s->config.rotation = 180;
                            else if (info->rotation & ECORE_X_RANDR_ORIENTATION_ROT_270)
                              s->config.rotation = 270;
                            minfo = ecore_x_randr_mode_info_get(root,
                                                                info->mode);
                            if (minfo)
                              {
                                 s->config.enabled = EINA_TRUE;
                                 s->config.mode.w = minfo->width;
                                 s->config.mode.h = minfo->height;
                                 s->config.mode.refresh =
                                   (double)minfo->dotClock /
                                   (double)(minfo->hTotal * minfo->vTotal);
                                 ecore_x_randr_mode_info_free(minfo);
                              }
                            printf("RRR: '%s' %i %i %ix%i\n",
                                   s->info.name,
                                   s->config.geom.x, s->config.geom.y,
                                   s->config.geom.w, s->config.geom.h);
                         }
                       if (info->rotations & ECORE_X_RANDR_ORIENTATION_ROT_0)
                         s->info.can_rot_0 = EINA_TRUE;
                       if (info->rotations & ECORE_X_RANDR_ORIENTATION_ROT_90)
                         s->info.can_rot_90 = EINA_TRUE;
                       if (info->rotations & ECORE_X_RANDR_ORIENTATION_ROT_180)
                         s->info.can_rot_180 = EINA_TRUE;
                       if (info->rotations & ECORE_X_RANDR_ORIENTATION_ROT_270)
                         s->info.can_rot_270 = EINA_TRUE;
                       if (cs)
                         {
                            if (cs->profile)
                              s->config.profile = strdup(cs->profile);
                            else
                              s->config.profile = NULL;
                            s->config.scale_multiplier = cs->scale_multiplier;
                         }
                    }
                  ecore_x_randr_crtc_info_free(info);
               }
          }
        zone = e_zone_for_id_get(s->id);
        if (zone && !zone->output) zone->output = s;
        r->screens = eina_list_append(r->screens, s);
     }

   free(outputs);
   free(crtcs);

   _info_relative_fixup(r);
   return r;
}

E_API void
e_comp_x_randr_screen_iface_set(void)
{
   if (e_comp->screen)
     CRI("CANNOT SET XIFACE; IFACE ALREADY EXISTS!");
   e_comp->screen = &xiface;
}

E_API Eina_Bool
e_comp_x_randr_canvas_new(Ecore_Window parent, int w, int h)
{
   Eina_Bool ret = EINA_TRUE;

   if (!getenv("ECORE_X_NO_XLIB"))
     {
        if (ecore_evas_engine_type_supported_get(ECORE_EVAS_ENGINE_OPENGL_X11))
          e_comp_gl_set(EINA_TRUE);
     }
   if (e_comp_gl_get() && (e_comp_config_get()->engine == E_COMP_ENGINE_GL))
     {
        int opt[20];
        int opt_i = 0;

        if (e_comp_config_get()->indirect)
          {
             opt[opt_i] = ECORE_EVAS_GL_X11_OPT_INDIRECT;
             opt_i++;
             opt[opt_i] = 1;
             opt_i++;
          }
        if (e_comp_config_get()->vsync)
          {
             opt[opt_i] = ECORE_EVAS_GL_X11_OPT_VSYNC;
             opt_i++;
             opt[opt_i] = 1;
             opt_i++;
          }
        if (e_comp_config_get()->swap_mode)
          {
             opt[opt_i] = ECORE_EVAS_GL_X11_OPT_SWAP_MODE;
             opt_i++;
             opt[opt_i] = e_comp_config_get()->swap_mode;
             opt_i++;
          }
        if (opt_i > 0)
          {
             opt[opt_i] = ECORE_EVAS_GL_X11_OPT_GL_DEPTH;
             opt_i++;
             opt[opt_i] = 24;
             opt_i++;
             opt[opt_i] = ECORE_EVAS_GL_X11_OPT_GL_STENCIL;
             opt_i++;
             opt[opt_i] = 8;
             opt_i++;
             opt[opt_i] = ECORE_EVAS_GL_X11_OPT_NONE;
             e_comp->ee = ecore_evas_gl_x11_options_new(NULL, parent, 0, 0, w, h, opt);
          }
        if (!e_comp->ee)
          e_comp->ee = ecore_evas_gl_x11_new(NULL, parent, 0, 0, w, h);
        if (e_comp->ee)
          {
             e_comp->gl = 1;
             ecore_evas_gl_x11_pre_post_swap_callback_set(e_comp->ee, e_comp, _e_comp_x_randr_pre_swap, NULL);
          }
     }
   if (!e_comp->ee)
     {
        e_comp->ee = ecore_evas_software_x11_new(NULL, parent, 0, 0, w, h);
        if (e_comp_config_get()->engine == E_COMP_ENGINE_GL)
          ret = EINA_FALSE;
        e_comp_gl_set(0);
        // tell elm and all elm apps to not allow acceleration since comp
        // can't do it (or doesn't want to), so this may cause issues in
        // gl drivers etc. - this addresses a vbox crash bug with vm
        // opengl acceleration
        elm_config_accel_preference_set("none");
        elm_config_accel_preference_override_set(EINA_TRUE);
        elm_config_all_flush();
        elm_config_save();
     }
   if (ret)
     e_comp->grab_cb = _e_comp_x_randr_grab_cb;
   return ret;
}

#include "e.h"

/* public variables */
E_API int E_EVENT_STATE_CHANGE = 0;

/* private stuff */

typedef struct _E_State_Item_Change_Callback
{
   const char  *glob;
   void       (*cb) (void *data, E_State_Item item, Eina_Bool del);
   void        *data;
   Eina_Bool    delete_me : 1;
} E_State_Item_Change_Callback;

static Eina_Hash *_e_state_items = NULL;
static int _e_state_change_callbacks_walking = 0;
static Eina_Bool _e_state_change_callbacks_need_del= EINA_FALSE;
static Eina_List *_e_state_change_callbacks = NULL;

static void
_state_item_free(void *data)
{
   E_State_Item *it = data;

   if (it->name) eina_stringshare_del(it->name);
   if (it->type == E_STATE_STRING) eina_stringshare_del(it->val.s);
   free(data);
}

static void
_state_event_free(void *data EINA_UNUSED, void *ev)
{
   E_Event_State *e = ev;
   Eina_List *l, *ll;
   E_State_Item_Change_Callback *cbit;

   _e_state_change_callbacks_walking++;
   EINA_LIST_FOREACH(_e_state_change_callbacks, l, cbit)
     {
        if (eina_fnmatch(cbit->glob, e->item.name, 0))
          {
             cbit->cb(cbit->data, e->item,
                      (e->event == E_STATE_DEL) ?
                      EINA_TRUE : EINA_FALSE);
          }
     }
   _e_state_change_callbacks_walking--;
   if ((_e_state_change_callbacks_walking == 0) &&
       (_e_state_change_callbacks_need_del))
     {
        EINA_LIST_FOREACH_SAFE(_e_state_change_callbacks, l, ll, cbit)
          {
             if (cbit->delete_me)
               {
                  _e_state_change_callbacks =
                    eina_list_remove_list(_e_state_change_callbacks, l);
                  eina_stringshare_del(cbit->glob);
                  free(cbit);
               }
          }
        _e_state_change_callbacks_need_del = EINA_FALSE;
     }
   if (e->item.name) eina_stringshare_del(e->item.name);
   if (e->item.type == E_STATE_STRING) eina_stringshare_del(e->item.val.s);
   free(e);
}

/* public functions */
EINTERN int
e_state_init(void)
{
   E_EVENT_STATE_CHANGE = ecore_event_type_new();

   _e_state_items = eina_hash_string_superfast_new(_state_item_free);

   return 1;
}

EINTERN int
e_state_shutdown(void)
{
   E_State_Item_Change_Callback *cbit;

   E_FREE_FUNC(_e_state_items, eina_hash_free);
   EINA_LIST_FREE(_e_state_change_callbacks, cbit)
     {
        eina_stringshare_del(cbit->glob);
        free(cbit);
     }
   return 1;
}

E_API void
e_state_item_change_callback_add(const char *glob, void (*cb) (void *data, E_State_Item item, Eina_Bool del), const void *data)
{
   E_State_Item_Change_Callback *cbit = calloc(1, sizeof(E_State_Item_Change_Callback));

   if (!cbit) return;
   cbit->glob = eina_stringshare_add(glob);
   if (!cbit->glob)
     {
        free(cbit);
        return;
     }
   cbit->cb = cb;
   cbit->data = (void *)data;
   _e_state_change_callbacks = eina_list_append(_e_state_change_callbacks, cbit);
}

E_API void
e_state_item_change_callback_del(const char *glob, void (*cb) (void *data, E_State_Item item, Eina_Bool del), const void *data)
{
   Eina_List *l;
   E_State_Item_Change_Callback *cbit;

   EINA_LIST_FOREACH(_e_state_change_callbacks, l, cbit)
     {
        if ((!strcmp(cbit->glob, glob)) &&
            (cbit->cb == cb) && (cbit->data == data))
          {
             if (_e_state_change_callbacks_walking > 0)
               {
                  cbit->delete_me = EINA_TRUE;
                  _e_state_change_callbacks_need_del = EINA_TRUE;
                  break;
               }
             _e_state_change_callbacks =
               eina_list_remove_list(_e_state_change_callbacks, l);
             eina_stringshare_del(cbit->glob);
             free(cbit);
             return;
          }
     }
}

E_API E_State_Item
e_state_item_get(const char *name)
{
   E_State_Item it = { NULL, E_STATE_UNKNOWN, { 0 } };
   E_State_Item *it2 = eina_hash_find(_e_state_items, name);

   if (it2) it = *it2;
   return it;
}

E_API void
e_state_item_set(E_State_Item it)
{
   E_State_Item *it2 = eina_hash_find(_e_state_items, it.name);
   Eina_Bool changed = EINA_FALSE;

   if (!it2)
     {
        it2 = calloc(1, sizeof(E_State_Item));
        if (!it2) return;
        it2->name = eina_stringshare_add(it.name);
        eina_hash_add(_e_state_items, it2->name, it2);
        changed = EINA_TRUE;
     }
   if (!changed)
     {
        if (it2->type != it.type) changed = EINA_TRUE;
     }
   if (!changed)
     {
        switch (it2->type)
          {
           case E_STATE_UNKNOWN:
             changed = EINA_TRUE;
             break;
           case E_STATE_BOOL:
             if (it2->val.b != it.val.b) changed = EINA_TRUE;
             break;
           case E_STATE_INT:
             if (it2->val.i != it.val.i) changed = EINA_TRUE;
             break;
           case E_STATE_UINT:
             if (it2->val.u != it.val.u) changed = EINA_TRUE;
             break;
           case E_STATE_DOUBLE:
             if (!EINA_FLT_EQ(it2->val.d, it.val.d)) changed = EINA_TRUE;
             break;
           case E_STATE_STRING:
             if (!((it2->val.s) && (it.val.s) &&
                   (!strcmp(it2->val.s, it.val.s))))
               changed = EINA_TRUE;
             break;
           default:
             break;
          }
     }
   if (changed)
     {
        E_Event_State *e;

        if (it2->type == E_STATE_STRING)
          {
             eina_stringshare_del(it2->val.s);
             it2->val.s = NULL;
          }
        it2->type = it.type;
        if (it2->type == E_STATE_STRING)
          it2->val.s = eina_stringshare_add(it.val.s);
        else
          it2->val = it.val;
        e = calloc(1, sizeof(E_Event_State));
        if (e)
          {
             e->event = E_STATE_CHANGE;
             e->item = *it2;
             e->item.name = eina_stringshare_add(e->item.name);
             if (e->item.type == E_STATE_STRING)
               e->item.val.s = eina_stringshare_add(e->item.val.s);
             ecore_event_add(E_EVENT_STATE_CHANGE, e, _state_event_free, NULL);
          }
     }
}

E_API void
e_state_item_del(const char *name)
{
   E_State_Item *it2 = eina_hash_find(_e_state_items, name);

   if (it2)
     {
        E_Event_State *e = calloc(1, sizeof(E_Event_State));

        if (e)
          {
             e->event = E_STATE_DEL;
             e->item = *it2;
             e->item.name = eina_stringshare_add(e->item.name);
             if (e->item.type == E_STATE_STRING)
               e->item.val.s = eina_stringshare_add(e->item.val.s);
             ecore_event_add(E_EVENT_STATE_CHANGE, e, _state_event_free, NULL);
          }
        eina_hash_del_by_key(_e_state_items, name);
     }
}

E_API Eina_Bool
e_state_item_val_bool_get(const char *name)
{
   E_State_Item it = e_state_item_get(name);

   if (it.type != E_STATE_BOOL) return EINA_FALSE;
   return it.val.b;
}

E_API int
e_state_item_val_int_get(const char *name)
{
   E_State_Item it = e_state_item_get(name);

   if (it.type != E_STATE_INT) return 0;
   return it.val.i;
}

E_API unsigned int
e_state_item_val_uint_get(const char *name)
{
   E_State_Item it = e_state_item_get(name);

   if (it.type != E_STATE_UINT) return 0;
   return it.val.u;
}

E_API double
e_state_item_val_double_get(const char *name)
{
   E_State_Item it = e_state_item_get(name);

   if (it.type != E_STATE_DOUBLE) return 0.0;
   return it.val.d;
}

E_API const char *
e_state_item_val_string_get(const char *name)
{
   E_State_Item it = e_state_item_get(name);

   if (it.type != E_STATE_STRING) return NULL;
   return it.val.s;
}

E_API void
e_state_item_val_bool_set(const char *name, Eina_Bool b)
{
   E_State_Item it;

   it.name = name;
   it.type = E_STATE_BOOL;
   it.val.b = b;
   e_state_item_set(it);
}

E_API void
e_state_item_val_int_set(const char *name, int i)
{
   E_State_Item it;

   it.name = name;
   it.type = E_STATE_INT;
   it.val.i = i;
   e_state_item_set(it);
}

E_API void
e_state_item_val_uint_set(const char *name, unsigned int u)
{
   E_State_Item it;

   it.name = name;
   it.type = E_STATE_UINT;
   it.val.u = u;
   e_state_item_set(it);
}

E_API void
e_state_item_val_double_set(const char *name, double d)
{
   E_State_Item it;

   it.name = name;
   it.type = E_STATE_DOUBLE;
   it.val.d = d;
   e_state_item_set(it);
}

E_API void
e_state_item_val_string_set(const char *name, const char *s)
{
   E_State_Item it;

   it.name = name;
   it.type = E_STATE_STRING;
   it.val.s = s;
   e_state_item_set(it);
}

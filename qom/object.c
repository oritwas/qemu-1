/*
 * QEMU Object Model
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/object.h"
#include "qemu-common.h"
#include "qapi/qapi-visit-core.h"
#include "qapi/string-input-visitor.h"
#include "qapi/string-output-visitor.h"

/* TODO: replace QObject with a simpler visitor to avoid a dependency
 * of the QOM core on QObject?  */
#include "qemu/qom-qobject.h"
#include "qobject.h"
#include "qbool.h"
#include "qint.h"
#include "qstring.h"

#define MAX_INTERFACES 32

typedef struct InterfaceImpl InterfaceImpl;
typedef struct TypeImpl TypeImpl;

struct InterfaceImpl
{
    const char *parent;
    void (*interface_initfn)(ObjectClass *class, void *data);
    TypeImpl *type;
};

struct TypeImpl
{
    const char *name;

    size_t class_size;

    size_t instance_size;

    void (*class_init)(ObjectClass *klass, void *data);
    void (*class_base_init)(ObjectClass *klass, void *data);
    void (*class_finalize)(ObjectClass *klass, void *data);

    void *class_data;

    void (*instance_init)(Object *obj);
    void (*instance_finalize)(Object *obj);

    bool abstract;

    const char *parent;
    TypeImpl *parent_type;

    ObjectClass *class;

    int num_interfaces;
    InterfaceImpl interfaces[MAX_INTERFACES];
};

typedef struct Interface
{
    Object parent;
    Object *obj;
} Interface;

#define INTERFACE(obj) OBJECT_CHECK(Interface, obj, TYPE_INTERFACE)

static Type type_interface;

static GHashTable *type_table_get(void)
{
    static GHashTable *type_table;

    if (type_table == NULL) {
        type_table = g_hash_table_new(g_str_hash, g_str_equal);
    }

    return type_table;
}

static void type_table_add(TypeImpl *ti)
{
    g_hash_table_insert(type_table_get(), (void *)ti->name, ti);
}

static TypeImpl *type_table_lookup(const char *name)
{
    return g_hash_table_lookup(type_table_get(), name);
}

static TypeImpl *type_register_internal(const TypeInfo *info)
{
    TypeImpl *ti = g_malloc0(sizeof(*ti));

    g_assert(info->name != NULL);

    if (type_table_lookup(info->name) != NULL) {
        fprintf(stderr, "Registering `%s' which already exists\n", info->name);
        abort();
    }

    ti->name = g_strdup(info->name);
    ti->parent = g_strdup(info->parent);

    ti->class_size = info->class_size;
    ti->instance_size = info->instance_size;

    ti->class_init = info->class_init;
    ti->class_base_init = info->class_base_init;
    ti->class_finalize = info->class_finalize;
    ti->class_data = info->class_data;

    ti->instance_init = info->instance_init;
    ti->instance_finalize = info->instance_finalize;

    ti->abstract = info->abstract;

    if (info->interfaces) {
        int i;

        for (i = 0; info->interfaces[i].type; i++) {
            ti->interfaces[i].parent = info->interfaces[i].type;
            ti->interfaces[i].interface_initfn = info->interfaces[i].interface_initfn;
            ti->num_interfaces++;
        }
    }

    type_table_add(ti);

    return ti;
}

TypeImpl *type_register(const TypeInfo *info)
{
    assert(info->parent);
    return type_register_internal(info);
}

TypeImpl *type_register_static(const TypeInfo *info)
{
    return type_register(info);
}

static TypeImpl *type_get_by_name(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    return type_table_lookup(name);
}

static TypeImpl *type_get_parent(TypeImpl *type)
{
    if (!type->parent_type && type->parent) {
        type->parent_type = type_get_by_name(type->parent);
        g_assert(type->parent_type != NULL);
    }

    return type->parent_type;
}

static bool type_has_parent(TypeImpl *type)
{
    return (type->parent != NULL);
}

static size_t type_class_get_size(TypeImpl *ti)
{
    if (ti->class_size) {
        return ti->class_size;
    }

    if (type_has_parent(ti)) {
        return type_class_get_size(type_get_parent(ti));
    }

    return sizeof(ObjectClass);
}

static size_t type_object_get_size(TypeImpl *ti)
{
    if (ti->instance_size) {
        return ti->instance_size;
    }

    if (type_has_parent(ti)) {
        return type_object_get_size(type_get_parent(ti));
    }

    return 0;
}

static void type_class_interface_init(TypeImpl *ti, InterfaceImpl *iface)
{
    TypeInfo info = {
        .instance_size = sizeof(Interface),
        .parent = iface->parent,
        .class_size = sizeof(InterfaceClass),
        .class_init = iface->interface_initfn,
        .abstract = true,
    };
    char *name = g_strdup_printf("<%s::%s>", ti->name, iface->parent);

    info.name = name;
    iface->type = type_register_internal(&info);
    g_free(name);
}

static void type_initialize(TypeImpl *ti)
{
    TypeImpl *parent;
    int i;

    if (ti->class) {
        return;
    }

    ti->class_size = type_class_get_size(ti);
    ti->instance_size = type_object_get_size(ti);

    ti->class = g_malloc0(ti->class_size);

    parent = type_get_parent(ti);
    if (parent) {
        type_initialize(parent);

        g_assert(parent->class_size <= ti->class_size);
        memcpy(ti->class, parent->class, parent->class_size);
    }

    ti->class->type = ti;

    while (parent) {
        if (parent->class_base_init) {
            parent->class_base_init(ti->class, ti->class_data);
        }
        parent = type_get_parent(parent);
    }

    for (i = 0; i < ti->num_interfaces; i++) {
        type_class_interface_init(ti, &ti->interfaces[i]);
    }

    if (ti->class_init) {
        ti->class_init(ti->class, ti->class_data);
    }
}

static void object_interface_init(Object *obj, InterfaceImpl *iface)
{
    TypeImpl *ti = iface->type;
    Interface *iface_obj;

    iface_obj = INTERFACE(object_new(ti->name));
    iface_obj->obj = obj;

    obj->interfaces = g_slist_prepend(obj->interfaces, iface_obj);
}

static void object_init_with_type(Object *obj, TypeImpl *ti)
{
    int i;

    if (type_has_parent(ti)) {
        object_init_with_type(obj, type_get_parent(ti));
    }

    for (i = 0; i < ti->num_interfaces; i++) {
        object_interface_init(obj, &ti->interfaces[i]);
    }

    if (ti->instance_init) {
        ti->instance_init(obj);
    }
}

void object_initialize_with_type(void *data, TypeImpl *type)
{
    Object *obj = data;

    g_assert(type != NULL);
    type_initialize(type);

    g_assert(type->instance_size >= sizeof(Object));
    g_assert(type->abstract == false);

    memset(obj, 0, type->instance_size);
    obj->class = type->class;
    QTAILQ_INIT(&obj->properties);
    object_init_with_type(obj, type);
}

void object_initialize(void *data, const char *typename)
{
    TypeImpl *type = type_get_by_name(typename);

    object_initialize_with_type(data, type);
}

static void object_property_del_all(Object *obj)
{
    while (!QTAILQ_EMPTY(&obj->properties)) {
        ObjectProperty *prop = QTAILQ_FIRST(&obj->properties);

        QTAILQ_REMOVE(&obj->properties, prop, node);

        if (prop->release) {
            prop->release(obj, prop->name, prop->opaque);
        }

        g_free(prop->name);
        g_free(prop->type);
        g_free(prop);
    }
}

static void object_property_del_child(Object *obj, Object *child, Error **errp)
{
    ObjectProperty *prop;

    QTAILQ_FOREACH(prop, &obj->properties, node) {
        if (strstart(prop->type, "child<", NULL) && prop->opaque == child) {
            object_property_del(obj, prop->name, errp);
            break;
        }
    }
}

static const char *_object_get_id(Object *obj)
{
    ObjectProperty *prop;

    QTAILQ_FOREACH(prop, &obj->properties, node) {
        if (strstart(prop->type, "child<", NULL) && prop->opaque == obj) {
            return prop->name;
        }
    }

    return "";
}

const char *object_get_id(Object *obj)
{
    return obj->class->get_id(obj);
}

void object_unparent(Object *obj)
{
    if (obj->parent) {
        object_property_del_child(obj->parent, obj, NULL);
    }
}

static void object_deinit(Object *obj, TypeImpl *type)
{
    if (type->instance_finalize) {
        type->instance_finalize(obj);
    }

    while (obj->interfaces) {
        Interface *iface_obj = obj->interfaces->data;
        obj->interfaces = g_slist_delete_link(obj->interfaces, obj->interfaces);
        object_delete(OBJECT(iface_obj));
    }

    if (type_has_parent(type)) {
        object_deinit(obj, type_get_parent(type));
    }

    object_unparent(obj);
}

void object_finalize(void *data)
{
    Object *obj = data;
    TypeImpl *ti = obj->class->type;

    object_deinit(obj, ti);
    object_property_del_all(obj);

    g_assert(obj->ref == 0);
}

Object *object_new_with_type(Type type)
{
    Object *obj;

    g_assert(type != NULL);
    type_initialize(type);

    obj = g_malloc(type->instance_size);
    object_initialize_with_type(obj, type);
    object_ref(obj);

    return obj;
}

Object *object_new(const char *typename)
{
    TypeImpl *ti = type_get_by_name(typename);

    return object_new_with_type(ti);
}

void object_delete(Object *obj)
{
    object_unref(obj);
    g_assert(obj->ref == 0);
    g_free(obj);
}

static bool type_is_ancestor(TypeImpl *type, TypeImpl *target_type)
{
    assert(target_type);

    /* Check if typename is a direct ancestor of type */
    while (type) {
        if (type == target_type) {
            return true;
        }

        type = type_get_parent(type);
    }

    return false;
}

static bool object_is_type(Object *obj, TypeImpl *target_type)
{
    return !target_type || type_is_ancestor(obj->class->type, target_type);
}

Object *object_dynamic_cast(Object *obj, const char *typename)
{
    TypeImpl *target_type = type_get_by_name(typename);
    GSList *i;

    /* Check if typename is a direct ancestor.  Special-case TYPE_OBJECT,
     * we want to go back from interfaces to the parent.
    */
    if (target_type && object_is_type(obj, target_type)) {
        return obj;
    }

    /* Check if obj is an interface and its containing object is a direct
     * ancestor of typename.  In principle we could do this test at the very
     * beginning of object_dynamic_cast, avoiding a second call to
     * object_is_type.  However, casting between interfaces is relatively
     * rare, and object_is_type(obj, type_interface) would fail almost always.
     *
     * Perhaps we could add a magic value to the object header for increased
     * (run-time) type safety and to speed up tests like this one.  If we ever
     * do that we can revisit the order here.
     */
    if (object_is_type(obj, type_interface)) {
        assert(!obj->interfaces);
        obj = INTERFACE(obj)->obj;
        if (object_is_type(obj, target_type)) {
            return obj;
        }
    }

    if (!target_type) {
        return obj;
    }

    /* Check if obj has an interface of typename */
    for (i = obj->interfaces; i; i = i->next) {
        Interface *iface = i->data;

        if (object_is_type(OBJECT(iface), target_type)) {
            return OBJECT(iface);
        }
    }

    return NULL;
}


Object *object_dynamic_cast_assert(Object *obj, const char *typename)
{
    Object *inst;

    inst = object_dynamic_cast(obj, typename);

    if (!inst) {
        fprintf(stderr, "Object %p is not an instance of type %s\n",
                obj, typename);
        abort();
    }

    return inst;
}

ObjectClass *object_class_dynamic_cast(ObjectClass *class,
                                       const char *typename)
{
    TypeImpl *target_type = type_get_by_name(typename);
    TypeImpl *type = class->type;

    while (type) {
        if (type == target_type) {
            return class;
        }

        type = type_get_parent(type);
    }

    return NULL;
}

ObjectClass *object_class_dynamic_cast_assert(ObjectClass *class,
                                              const char *typename)
{
    ObjectClass *ret = object_class_dynamic_cast(class, typename);

    if (!ret) {
        fprintf(stderr, "Object %p is not an instance of type %s\n",
                class, typename);
        abort();
    }

    return ret;
}

const char *object_get_typename(Object *obj)
{
    return obj->class->type->name;
}

ObjectClass *object_get_class(Object *obj)
{
    return obj->class;
}

const char *object_class_get_name(ObjectClass *klass)
{
    return klass->type->name;
}

ObjectClass *object_class_by_name(const char *typename)
{
    TypeImpl *type = type_get_by_name(typename);

    if (!type) {
        return NULL;
    }

    type_initialize(type);

    return type->class;
}

ObjectClass *object_class_get_parent(ObjectClass *class)
{
    TypeImpl *type = type_get_parent(class->type);

    if (!type) {
        return NULL;
    }

    type_initialize(type);

    return type->class;
}

typedef struct OCFData
{
    void (*fn)(ObjectClass *klass, void *opaque);
    const char *implements_type;
    bool include_abstract;
    void *opaque;
} OCFData;

static void object_class_foreach_tramp(gpointer key, gpointer value,
                                       gpointer opaque)
{
    OCFData *data = opaque;
    TypeImpl *type = value;
    ObjectClass *k;

    type_initialize(type);
    k = type->class;

    if (!data->include_abstract && type->abstract) {
        return;
    }

    if (data->implements_type && 
        !object_class_dynamic_cast(k, data->implements_type)) {
        return;
    }

    data->fn(k, data->opaque);
}

void object_class_foreach(void (*fn)(ObjectClass *klass, void *opaque),
                          const char *implements_type, bool include_abstract,
                          void *opaque)
{
    OCFData data = { fn, implements_type, include_abstract, opaque };

    g_hash_table_foreach(type_table_get(), object_class_foreach_tramp, &data);
}

int object_child_foreach(Object *obj, int (*fn)(Object *child, void *opaque),
                         void *opaque)
{
    ObjectProperty *prop;
    int ret = 0;

    QTAILQ_FOREACH(prop, &obj->properties, node) {
        if (strstart(prop->type, "child<", NULL)) {
            ret = fn(prop->opaque, opaque);
            if (ret != 0) {
                break;
            }
        }
    }
    return ret;
}

static void object_class_get_list_tramp(ObjectClass *klass, void *opaque)
{
    GSList **list = opaque;

    *list = g_slist_prepend(*list, klass);
}

GSList *object_class_get_list(const char *implements_type,
                              bool include_abstract)
{
    GSList *list = NULL;

    object_class_foreach(object_class_get_list_tramp,
                         implements_type, include_abstract, &list);
    return list;
}

void object_ref(Object *obj)
{
    obj->ref++;
}

void object_unref(Object *obj)
{
    g_assert(obj->ref > 0);
    obj->ref--;

    /* parent always holds a reference to its children */
    if (obj->ref == 0) {
        object_finalize(obj);
    }
}

void object_property_add(Object *obj, const char *name, const char *type,
                         ObjectPropertyAccessor *get,
                         ObjectPropertyAccessor *set,
                         ObjectPropertyRelease *release,
                         void *opaque, Error **errp)
{
    ObjectProperty *prop = g_malloc0(sizeof(*prop));

    prop->name = g_strdup(name);
    prop->type = g_strdup(type);

    prop->get = get;
    prop->set = set;
    prop->release = release;
    prop->opaque = opaque;

    QTAILQ_INSERT_TAIL(&obj->properties, prop, node);
}

ObjectProperty *object_property_find(Object *obj, const char *name, Error **errp)
{
    ObjectProperty *prop;

    QTAILQ_FOREACH(prop, &obj->properties, node) {
        if (strcmp(prop->name, name) == 0) {
            return prop;
        }
    }

    error_set(errp, QERR_PROPERTY_NOT_FOUND, object_get_id(obj), name);
    return NULL;
}

void object_property_del(Object *obj, const char *name, Error **errp)
{
    ObjectProperty *prop = object_property_find(obj, name, errp);
    if (prop == NULL) {
        return;
    }

    if (prop == NULL) {
        error_set(errp, QERR_PROPERTY_NOT_FOUND, "", name);
        return;
    }

    if (prop->release) {
        prop->release(obj, name, prop->opaque);
    }

    QTAILQ_REMOVE(&obj->properties, prop, node);

    g_free(prop->name);
    g_free(prop->type);
    g_free(prop);
}

void object_property_get(Object *obj, Visitor *v, const char *name,
                         Error **errp)
{
    ObjectProperty *prop = object_property_find(obj, name, errp);
    if (prop == NULL) {
        return;
    }

    if (!prop->get) {
        error_set(errp, QERR_PERMISSION_DENIED);
    } else {
        prop->get(obj, v, prop->opaque, name, errp);
    }
}

void object_property_set(Object *obj, Visitor *v, const char *name,
                         Error **errp)
{
    ObjectProperty *prop = object_property_find(obj, name, errp);
    if (prop == NULL) {
        return;
    }

    if (!prop->set) {
        error_set(errp, QERR_PERMISSION_DENIED);
    } else {
        prop->set(obj, v, prop->opaque, name, errp);
    }
}

void object_property_set_str(Object *obj, const char *value,
                             const char *name, Error **errp)
{
    QString *qstr = qstring_from_str(value);
    object_property_set_qobject(obj, QOBJECT(qstr), name, errp);

    QDECREF(qstr);
}

char *object_property_get_str(Object *obj, const char *name,
                              Error **errp)
{
    QObject *ret = object_property_get_qobject(obj, name, errp);
    QString *qstring;
    char *retval;

    if (!ret) {
        return NULL;
    }
    qstring = qobject_to_qstring(ret);
    if (!qstring) {
        error_set(errp, QERR_INVALID_PARAMETER_TYPE, name, "string");
        retval = NULL;
    } else {
        retval = g_strdup(qstring_get_str(qstring));
    }

    QDECREF(qstring);
    return retval;
}

void object_property_set_link(Object *obj, Object *value,
                              const char *name, Error **errp)
{
    object_property_set_str(obj, object_get_canonical_path(value),
                            name, errp);
}

Object *object_property_get_link(Object *obj, const char *name,
                                 Error **errp)
{
    char *str = object_property_get_str(obj, name, errp);
    Object *target = NULL;

    if (str && *str) {
        target = object_resolve_path(str, NULL);
        if (!target) {
            error_set(errp, QERR_DEVICE_NOT_FOUND, str);
        }
    }

    g_free(str);
    return target;
}

void object_property_set_bool(Object *obj, bool value,
                              const char *name, Error **errp)
{
    QBool *qbool = qbool_from_int(value);
    object_property_set_qobject(obj, QOBJECT(qbool), name, errp);

    QDECREF(qbool);
}

bool object_property_get_bool(Object *obj, const char *name,
                              Error **errp)
{
    QObject *ret = object_property_get_qobject(obj, name, errp);
    QBool *qbool;
    bool retval;

    if (!ret) {
        return false;
    }
    qbool = qobject_to_qbool(ret);
    if (!qbool) {
        error_set(errp, QERR_INVALID_PARAMETER_TYPE, name, "boolean");
        retval = false;
    } else {
        retval = qbool_get_int(qbool);
    }

    QDECREF(qbool);
    return retval;
}

void object_property_set_int(Object *obj, int64_t value,
                             const char *name, Error **errp)
{
    QInt *qint = qint_from_int(value);
    object_property_set_qobject(obj, QOBJECT(qint), name, errp);

    QDECREF(qint);
}

int64_t object_property_get_int(Object *obj, const char *name,
                                Error **errp)
{
    QObject *ret = object_property_get_qobject(obj, name, errp);
    QInt *qint;
    int64_t retval;

    if (!ret) {
        return -1;
    }
    qint = qobject_to_qint(ret);
    if (!qint) {
        error_set(errp, QERR_INVALID_PARAMETER_TYPE, name, "int");
        retval = -1;
    } else {
        retval = qint_get_int(qint);
    }

    QDECREF(qint);
    return retval;
}

void object_property_parse(Object *obj, const char *string,
                           const char *name, Error **errp)
{
    StringInputVisitor *mi;
    mi = string_input_visitor_new(string);
    object_property_set(obj, string_input_get_visitor(mi), name, errp);

    string_input_visitor_cleanup(mi);
}

char *object_property_print(Object *obj, const char *name,
                            Error **errp)
{
    StringOutputVisitor *mo;
    char *string;

    mo = string_output_visitor_new();
    object_property_get(obj, string_output_get_visitor(mo), name, errp);
    string = string_output_get_string(mo);
    string_output_visitor_cleanup(mo);
    return string;
}

const char *object_property_get_type(Object *obj, const char *name, Error **errp)
{
    ObjectProperty *prop = object_property_find(obj, name, errp);
    if (prop == NULL) {
        return NULL;
    }

    return prop->type;
}

Object *object_get_root(void)
{
    static Object *root;

    if (!root) {
        root = object_new("container");
    }

    return root;
}

static void object_get_child_property(Object *obj, Visitor *v, void *opaque,
                                      const char *name, Error **errp)
{
    Object *child = opaque;
    gchar *path;

    path = object_get_canonical_path(child);
    visit_type_str(v, &path, name, errp);
    g_free(path);
}

static void object_finalize_child_property(Object *obj, const char *name,
                                           void *opaque)
{
    Object *child = opaque;

    object_unref(child);
}

void object_property_add_child(Object *obj, const char *name,
                               Object *child, Error **errp)
{
    gchar *type;

    /* Registering an interface object in the composition tree will mightily
     * confuse object_get_canonical_path (which, on the other hand, knows how
     * to get the canonical path of an interface object).
     */
    assert(!object_is_type(obj, type_interface));

    type = g_strdup_printf("child<%s>", object_get_typename(OBJECT(child)));

    object_property_add(obj, name, type, object_get_child_property,
                        NULL, object_finalize_child_property, child, errp);

    object_ref(child);
    g_assert(child->parent == NULL);
    child->parent = obj;

    g_free(type);
}

static void object_get_link_property(Object *obj, Visitor *v, void *opaque,
                                     const char *name, Error **errp)
{
    Object **child = opaque;
    gchar *path;

    if (*child) {
        path = object_get_canonical_path(*child);
        visit_type_str(v, &path, name, errp);
        g_free(path);
    } else {
        path = (gchar *)"";
        visit_type_str(v, &path, name, errp);
    }
}

static void object_set_link_property(Object *obj, Visitor *v, void *opaque,
                                     const char *name, Error **errp)
{
    Object **child = opaque;
    Object *old_target;
    bool ambiguous = false;
    const char *type;
    char *path;
    gchar *target_type;

    type = object_property_get_type(obj, name, NULL);

    visit_type_str(v, &path, name, errp);

    old_target = *child;
    *child = NULL;

    if (strcmp(path, "") != 0) {
        Object *target;

        /* Go from link<FOO> to FOO.  */
        target_type = g_strndup(&type[5], strlen(type) - 6);
        target = object_resolve_path_type(path, target_type, &ambiguous);

        if (ambiguous) {
            error_set(errp, QERR_AMBIGUOUS_PATH, path);
        } else if (target) {
            object_ref(target);
            *child = target;
        } else {
            target = object_resolve_path(path, &ambiguous);
            if (target || ambiguous) {
                error_set(errp, QERR_INVALID_PARAMETER_TYPE, name, target_type);
            } else {
                error_set(errp, QERR_DEVICE_NOT_FOUND, path);
            }
        }
        g_free(target_type);
    }

    g_free(path);

    if (old_target != NULL) {
        object_unref(old_target);
    }
}

void object_property_add_link(Object *obj, const char *name,
                              const char *type, Object **child,
                              Error **errp)
{
    gchar *full_type;

    full_type = g_strdup_printf("link<%s>", type);

    object_property_add(obj, name, full_type,
                        object_get_link_property,
                        object_set_link_property,
                        NULL, child, errp);

    g_free(full_type);
}

gchar *object_get_canonical_path(Object *obj)
{
    Object *root = object_get_root();
    char *newpath = NULL, *path = NULL;

    if (object_is_type(obj, type_interface)) {
        obj = INTERFACE(obj)->obj;
    }

    while (obj != root) {
        ObjectProperty *prop = NULL;

        g_assert(obj->parent != NULL);

        QTAILQ_FOREACH(prop, &obj->parent->properties, node) {
            if (!strstart(prop->type, "child<", NULL)) {
                continue;
            }

            if (prop->opaque == obj) {
                if (path) {
                    newpath = g_strdup_printf("%s/%s", prop->name, path);
                    g_free(path);
                    path = newpath;
                } else {
                    path = g_strdup(prop->name);
                }
                break;
            }
        }

        g_assert(prop != NULL);

        obj = obj->parent;
    }

    newpath = g_strdup_printf("/%s", path);
    g_free(path);

    return newpath;
}

Object *object_resolve_path_component(Object *parent, gchar *part)
{
    ObjectProperty *prop = object_property_find(parent, part, NULL);
    if (prop == NULL) {
        return NULL;
    }

    if (strstart(prop->type, "link<", NULL)) {
        return *(Object **)prop->opaque;
    } else if (strstart(prop->type, "child<", NULL)) {
        return prop->opaque;
    } else {
        return NULL;
    }
}

static Object *object_resolve_abs_path(Object *parent,
                                          gchar **parts,
                                          const char *typename,
                                          int index)
{
    Object *child;

    if (parts[index] == NULL) {
        return object_dynamic_cast(parent, typename);
    }

    if (strcmp(parts[index], "") == 0) {
        return object_resolve_abs_path(parent, parts, typename, index + 1);
    }

    child = object_resolve_path_component(parent, parts[index]);
    if (!child) {
        return NULL;
    }

    return object_resolve_abs_path(child, parts, typename, index + 1);
}

static Object *object_resolve_partial_path(Object *parent,
                                              gchar **parts,
                                              const char *typename,
                                              bool *ambiguous)
{
    Object *obj;
    ObjectProperty *prop;

    obj = object_resolve_abs_path(parent, parts, typename, 0);

    QTAILQ_FOREACH(prop, &parent->properties, node) {
        Object *found;

        if (!strstart(prop->type, "child<", NULL)) {
            continue;
        }

        found = object_resolve_partial_path(prop->opaque, parts,
                                            typename, ambiguous);
        if (found) {
            if (obj) {
                if (ambiguous) {
                    *ambiguous = true;
                }
                return NULL;
            }
            obj = found;
        }

        if (ambiguous && *ambiguous) {
            return NULL;
        }
    }

    return obj;
}

Object *object_resolve_path_type(const char *path, const char *typename,
                                 bool *ambiguous)
{
    bool partial_path = true;
    Object *obj;
    gchar **parts;

    parts = g_strsplit(path, "/", 0);
    if (parts == NULL || parts[0] == NULL) {
        g_strfreev(parts);
        return object_get_root();
    }

    if (strcmp(parts[0], "") == 0) {
        partial_path = false;
    }

    if (partial_path) {
        if (ambiguous) {
            *ambiguous = false;
        }
        obj = object_resolve_partial_path(object_get_root(), parts,
                                          typename, ambiguous);
    } else {
        obj = object_resolve_abs_path(object_get_root(), parts, typename, 1);
    }

    g_strfreev(parts);

    return obj;
}

Object *object_resolve_path(const char *path, bool *ambiguous)
{
    return object_resolve_path_type(path, TYPE_OBJECT, ambiguous);
}

typedef struct StringProperty
{
    char *(*get)(Object *, Error **);
    void (*set)(Object *, const char *, Error **);
} StringProperty;

static void property_get_str(Object *obj, Visitor *v, void *opaque,
                             const char *name, Error **errp)
{
    StringProperty *prop = opaque;
    char *value;

    value = prop->get(obj, errp);
    if (value) {
        visit_type_str(v, &value, name, errp);
        g_free(value);
    }
}

static void property_set_str(Object *obj, Visitor *v, void *opaque,
                             const char *name, Error **errp)
{
    StringProperty *prop = opaque;
    char *value;
    Error *local_err = NULL;

    visit_type_str(v, &value, name, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    prop->set(obj, value, errp);
    g_free(value);
}

static void property_release_str(Object *obj, const char *name,
                                 void *opaque)
{
    StringProperty *prop = opaque;
    g_free(prop);
}

void object_property_add_str(Object *obj, const char *name,
                           char *(*get)(Object *, Error **),
                           void (*set)(Object *, const char *, Error **),
                           Error **errp)
{
    StringProperty *prop = g_malloc0(sizeof(*prop));

    prop->get = get;
    prop->set = set;

    object_property_add(obj, name, "string",
                        get ? property_get_str : NULL,
                        set ? property_set_str : NULL,
                        property_release_str,
                        prop, errp);
}

static char *qdev_get_type(Object *obj, Error **errp)
{
    return g_strdup(object_get_typename(obj));
}

bool object_is_realized(Object *obj)
{
    return obj->state == OBJECT_STATE_REALIZED;
}

static void object_instance_init(Object *obj)
{
    object_property_add_str(obj, "type", qdev_get_type, NULL, NULL);
    obj->state = OBJECT_STATE_CREATED;
}

static void object_class_init(ObjectClass *klass, void *class_data)
{
    klass->get_id = _object_get_id;
}

static void register_types(void)
{
    static TypeInfo interface_info = {
        .name = TYPE_INTERFACE,
        .instance_size = sizeof(Interface),
        .abstract = true,
    };

    static TypeInfo object_info = {
        .name = TYPE_OBJECT,
        .instance_size = sizeof(Object),
        .instance_init = object_instance_init,
        .class_init = object_class_init,
        .abstract = true,
    };

    type_interface = type_register_internal(&interface_info);
    type_register_internal(&object_info);
}

type_init(register_types)

void *object_get_prop_ptr(Object *obj, Property *prop)
{
    void *ptr = obj;
    ptr += prop->offset;
    return ptr;
}

static uint32_t get_prop_mask(Property *prop)
{
    assert(prop->info == &qdev_prop_bit);
    return 0x1 << prop->bitnr;
}

static void bit_prop_set(Object *obj, Property *props, bool val)
{
    uint32_t *p = object_get_prop_ptr(obj, props);
    uint32_t mask = get_prop_mask(props);
    if (val)
        *p |= mask;
    else
        *p &= ~mask;
}

/* Bit */

static int print_bit(Object *obj, Property *prop, char *dest, size_t len)
{
    uint32_t *p = object_get_prop_ptr(obj, prop);
    return snprintf(dest, len, (*p & get_prop_mask(prop)) ? "on" : "off");
}

static void get_bit(Object *obj, Visitor *v, void *opaque,
                    const char *name, Error **errp)
{
    Property *prop = opaque;
    uint32_t *p = object_get_prop_ptr(obj, prop);
    bool value = (*p & get_prop_mask(prop)) != 0;

    visit_type_bool(v, &value, name, errp);
}

static void set_bit(Object *obj, Visitor *v, void *opaque,
                    const char *name, Error **errp)
{
    Property *prop = opaque;
    Error *local_err = NULL;
    bool value;

    if (object_is_realized(obj)) {
        error_set(errp, QERR_PERMISSION_DENIED);
        return;
    }

    visit_type_bool(v, &value, name, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    bit_prop_set(obj, prop, value);
}

PropertyInfo qdev_prop_bit = {
    .name  = "boolean",
    .legacy_name  = "on/off",
    .print = print_bit,
    .get   = get_bit,
    .set   = set_bit,
};

/* --- 8bit integer --- */

static void get_uint8(Object *obj, Visitor *v, void *opaque,
                      const char *name, Error **errp)
{
    Property *prop = opaque;
    uint8_t *ptr = object_get_prop_ptr(obj, prop);

    visit_type_uint8(v, ptr, name, errp);
}

static void set_uint8(Object *obj, Visitor *v, void *opaque,
                      const char *name, Error **errp)
{
    Property *prop = opaque;
    uint8_t *ptr = object_get_prop_ptr(obj, prop);

    if (object_is_realized(obj)) {
        error_set(errp, QERR_PERMISSION_DENIED);
        return;
    }

    visit_type_uint8(v, ptr, name, errp);
}

PropertyInfo qdev_prop_uint8 = {
    .name  = "uint8",
    .get   = get_uint8,
    .set   = set_uint8,
};

/* --- 8bit hex value --- */

static int parse_hex8(Object *obj, Property *prop, const char *str)
{
    uint8_t *ptr = object_get_prop_ptr(obj, prop);
    char *end;

    if (str[0] != '0' || str[1] != 'x') {
        return -EINVAL;
    }

    *ptr = strtoul(str, &end, 16);
    if ((*end != '\0') || (end == str)) {
        return -EINVAL;
    }

    return 0;
}

static int print_hex8(Object *obj, Property *prop, char *dest, size_t len)
{
    uint8_t *ptr = object_get_prop_ptr(obj, prop);
    return snprintf(dest, len, "0x%" PRIx8, *ptr);
}

PropertyInfo qdev_prop_hex8 = {
    .name  = "uint8",
    .legacy_name  = "hex8",
    .parse = parse_hex8,
    .print = print_hex8,
    .get   = get_uint8,
    .set   = set_uint8,
};

/* --- 16bit integer --- */

static void get_uint16(Object *obj, Visitor *v, void *opaque,
                       const char *name, Error **errp)
{
    Property *prop = opaque;
    uint16_t *ptr = object_get_prop_ptr(obj, prop);

    visit_type_uint16(v, ptr, name, errp);
}

static void set_uint16(Object *obj, Visitor *v, void *opaque,
                       const char *name, Error **errp)
{
    Property *prop = opaque;
    uint16_t *ptr = object_get_prop_ptr(obj, prop);

    if (object_is_realized(obj)) {
        error_set(errp, QERR_PERMISSION_DENIED);
        return;
    }

    visit_type_uint16(v, ptr, name, errp);
}

PropertyInfo qdev_prop_uint16 = {
    .name  = "uint16",
    .get   = get_uint16,
    .set   = set_uint16,
};

/* --- 32bit integer --- */

static void get_uint32(Object *obj, Visitor *v, void *opaque,
                       const char *name, Error **errp)
{
    Property *prop = opaque;
    uint32_t *ptr = object_get_prop_ptr(obj, prop);

    visit_type_uint32(v, ptr, name, errp);
}

static void set_uint32(Object *obj, Visitor *v, void *opaque,
                       const char *name, Error **errp)
{
    Property *prop = opaque;
    uint32_t *ptr = object_get_prop_ptr(obj, prop);

    if (object_is_realized(obj)) {
        error_set(errp, QERR_PERMISSION_DENIED);
        return;
    }

    visit_type_uint32(v, ptr, name, errp);
}

static void get_int32(Object *obj, Visitor *v, void *opaque,
                      const char *name, Error **errp)
{
    Property *prop = opaque;
    int32_t *ptr = object_get_prop_ptr(obj, prop);

    visit_type_int32(v, ptr, name, errp);
}

static void set_int32(Object *obj, Visitor *v, void *opaque,
                      const char *name, Error **errp)
{
    Property *prop = opaque;
    int32_t *ptr = object_get_prop_ptr(obj, prop);

    if (object_is_realized(obj)) {
        error_set(errp, QERR_PERMISSION_DENIED);
        return;
    }

    visit_type_int32(v, ptr, name, errp);
}

PropertyInfo qdev_prop_uint32 = {
    .name  = "uint32",
    .get   = get_uint32,
    .set   = set_uint32,
};

PropertyInfo qdev_prop_int32 = {
    .name  = "int32",
    .get   = get_int32,
    .set   = set_int32,
};

/* --- 32bit hex value --- */

static int parse_hex32(Object *obj, Property *prop, const char *str)
{
    uint32_t *ptr = object_get_prop_ptr(obj, prop);
    char *end;

    if (str[0] != '0' || str[1] != 'x') {
        return -EINVAL;
    }

    *ptr = strtoul(str, &end, 16);
    if ((*end != '\0') || (end == str)) {
        return -EINVAL;
    }

    return 0;
}

static int print_hex32(Object *obj, Property *prop, char *dest, size_t len)
{
    uint32_t *ptr = object_get_prop_ptr(obj, prop);
    return snprintf(dest, len, "0x%" PRIx32, *ptr);
}

PropertyInfo qdev_prop_hex32 = {
    .name  = "uint32",
    .legacy_name  = "hex32",
    .parse = parse_hex32,
    .print = print_hex32,
    .get   = get_uint32,
    .set   = set_uint32,
};

/* --- 64bit integer --- */

static void get_uint64(Object *obj, Visitor *v, void *opaque,
                       const char *name, Error **errp)
{
    Property *prop = opaque;
    uint64_t *ptr = object_get_prop_ptr(obj, prop);

    visit_type_uint64(v, ptr, name, errp);
}

static void set_uint64(Object *obj, Visitor *v, void *opaque,
                       const char *name, Error **errp)
{
    Property *prop = opaque;
    uint64_t *ptr = object_get_prop_ptr(obj, prop);

    if (object_is_realized(obj)) {
        error_set(errp, QERR_PERMISSION_DENIED);
        return;
    }

    visit_type_uint64(v, ptr, name, errp);
}

PropertyInfo qdev_prop_uint64 = {
    .name  = "uint64",
    .get   = get_uint64,
    .set   = set_uint64,
};

/* --- 64bit hex value --- */

static int parse_hex64(Object *obj, Property *prop, const char *str)
{
    uint64_t *ptr = object_get_prop_ptr(obj, prop);
    char *end;

    if (str[0] != '0' || str[1] != 'x') {
        return -EINVAL;
    }

    *ptr = strtoull(str, &end, 16);
    if ((*end != '\0') || (end == str)) {
        return -EINVAL;
    }

    return 0;
}

static int print_hex64(Object *obj, Property *prop, char *dest, size_t len)
{
    uint64_t *ptr = object_get_prop_ptr(obj, prop);
    return snprintf(dest, len, "0x%" PRIx64, *ptr);
}

PropertyInfo qdev_prop_hex64 = {
    .name  = "uint64",
    .legacy_name  = "hex64",
    .parse = parse_hex64,
    .print = print_hex64,
    .get   = get_uint64,
    .set   = set_uint64,
};

/* --- string --- */

static void release_string(Object *obj, const char *name, void *opaque)
{
    Property *prop = opaque;
    g_free(*(char **)object_get_prop_ptr(obj, prop));
}

static int print_string(Object *obj, Property *prop, char *dest, size_t len)
{
    char **ptr = object_get_prop_ptr(obj, prop);
    if (!*ptr)
        return snprintf(dest, len, "<null>");
    return snprintf(dest, len, "\"%s\"", *ptr);
}

static void get_string(Object *obj, Visitor *v, void *opaque,
                       const char *name, Error **errp)
{
    Property *prop = opaque;
    char **ptr = object_get_prop_ptr(obj, prop);

    if (!*ptr) {
        char *str = (char *)"";
        visit_type_str(v, &str, name, errp);
    } else {
        visit_type_str(v, ptr, name, errp);
    }
}

static void set_string(Object *obj, Visitor *v, void *opaque,
                       const char *name, Error **errp)
{
    Property *prop = opaque;
    char **ptr = object_get_prop_ptr(obj, prop);
    Error *local_err = NULL;
    char *str;

    if (object_is_realized(obj)) {
        error_set(errp, QERR_PERMISSION_DENIED);
        return;
    }

    visit_type_str(v, &str, name, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    if (*ptr) {
        g_free(*ptr);
    }
    *ptr = str;
}

PropertyInfo qdev_prop_string = {
    .name  = "string",
    .print = print_string,
    .release = release_string,
    .get   = get_string,
    .set   = set_string,
};


/* --- enums --- */

void property_get_enum(Object *obj, Visitor *v, void *opaque,
                       const char *name, Error **errp)
{
    Property *prop = opaque;
    int *ptr = object_get_prop_ptr(obj, prop);

    visit_type_enum(v, ptr, prop->info->enum_table,
                    prop->info->name, prop->name, errp);
}

void property_set_enum(Object *obj, Visitor *v, void *opaque,
                       const char *name, Error **errp)
{
    Property *prop = opaque;
    int *ptr = object_get_prop_ptr(obj, prop);

    if (object_is_realized(obj)) {
        error_set(errp, QERR_PERMISSION_DENIED);
        return;
    }

    visit_type_enum(v, ptr, prop->info->enum_table,
                    prop->info->name, prop->name, errp);
}


/**
 * @object_property_add_static - add a @Property to a device.
 *
 * Static properties access data in a struct.  The actual type of the
 * property and the field depends on the property type.
 */
void object_property_add_static(Object *obj, Property *prop,
                                Error **errp)
{
    Error *local_err = NULL;

    /*
     * TODO qdev_prop_ptr does not have getters or setters.  It must
     * go now that it can be replaced with links.  The test should be
     * removed along with it: all static properties are read/write.
     */
    if (!prop->info->get && !prop->info->set) {
        return;
    }

    object_property_add(obj, prop->name, prop->info->name,
                        prop->info->get, prop->info->set,
                        prop->info->release,
                        prop, &local_err);

    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    if (prop->qtype == QTYPE_NONE) {
        return;
    }

    if (prop->qtype == QTYPE_QBOOL) {
        object_property_set_bool(obj, prop->defval, prop->name, &local_err);
    } else if (prop->info->enum_table) {
        object_property_set_str(obj, prop->info->enum_table[prop->defval],
                                prop->name, &local_err);
    } else if (prop->qtype == QTYPE_QINT) {
        object_property_set_int(obj, prop->defval, prop->name, &local_err);
    }
    assert_no_error(local_err);
}

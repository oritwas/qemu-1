/*
 * QEMU IDL Macros/stubs
 *
 * See docs/qidl.txt for usage information.
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Michael Roth    <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPLv2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QIDL_H
#define QIDL_H

#include <glib.h>
#include "qapi/qapi-visit-core.h"

/* must be "called" in any C files that make use of QIDL-generated code */
#define QIDL_ENABLE()

/* we pass the code through the preprocessor with QIDL_GEN defined to parse
 * structures as they'd appear after preprocessing, and use the following
 * definitions mostly to re-insert the initial macros/annotations so they
 * stick around for the parser to process
 */
#ifdef QIDL_GEN

#define QIDL(...) QIDL(__VA_ARGS__)
#define QIDL_START(name, ...) QIDL_START(name, ##__VA_ARGS__)

#else

#define QIDL(...)
#ifdef QIDL_ENABLED
#define QIDL_START(name, ...) \
    static struct { \
        void (*visitor)(Visitor *, struct name **, const char *, Error **); \
        const char *schema_json_text; \
        struct Object *schema_obj; \
        struct Property *properties; \
    } qidl_data_##name;
#else
#define QIDL_START(name,  ...)
#endif /* QIDL_ENABLED */

#endif /* QIDL_GEN */

/* QIDL annotations/markers
 *
 * q_immutable: state is fully restorable via device
 *   [re-]initialization/realization
 *
 * q_derived: state can be fully reconstructed from other fields (and will be,
 *   via [re-]initialization of the device or a separate hook)
 *
 * q_broken: state should (or possibly should) be saved, but isn't. mostly an aid
 *   for device developers having issues with serialization of a particular
 *   field, committed code should contain these except in special circumstances
 *
 * q_optional: <field> should only be serialized if the field by the name of
 *   has_<field> is true
 *
 * q_elsewhere: state should be serialized, but is done so elsewhere (for
 *   instance, by another device with a pointer to the same data)
 *
 * q_size(field): for static/dynamically-allocated arrays. specifies the field
 *   in the structure containing the number of elements that should be
 *   serialized. if argument is wrapped in parenthesis it is instead interpreted
 *   as an expression that should be invaluated to determine the size.
 *
 * q_property(<property name> [, <default value>]): specifies that field is a
 *   qdev-style property. all properties of the struct are then accessible via
 *   QIDL_PROPERTIES(<device name>) macro.
 */
#define q_immutable QIDL(immutable)
#define q_derived QIDL(derived)
#define q_broken QIDL(broken)
#define q_optional QIDL(optional)
#define q_elsewhere QIDL(elsewhere)
#define q_size(...) QIDL(size_is, ##__VA_ARGS__)
#define q_property(name, ...) QIDL(property, name, ##__VA_ARGS__)

#define QIDL_DECLARE(name, ...) \
    QIDL_START(name, ##__VA_ARGS__) \
    struct name

#define QIDL_VISIT_TYPE(name, v, s, f, e) \
    g_assert(qidl_data_##name.visitor); \
    qidl_data_##name.visitor(v, s, NULL, e)

#define QIDL_SCHEMA_ADD_LINK(name, obj, path, errp) \
    g_assert(qidl_data_##name.schema_obj); \
    object_property_add_link(obj, path, "container", \
                             &qidl_data_##name.schema_obj, errp)

#define QIDL_PROPERTIES(name) \
    qidl_data_##name.properties

#endif

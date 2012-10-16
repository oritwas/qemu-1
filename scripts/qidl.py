#
# QIDL Code Generator
#
# Copyright IBM, Corp. 2012
#
# Authors:
#  Anthony Liguori <aliguori@us.ibm.com>
#  Michael Roth    <mdroth@linux.vnet.ibm.com>
#
# This work is licensed under the terms of the GNU GPLv2 or later.
# See the COPYING file in the top-level directory.

from ordereddict import OrderedDict
from qidl_parser import parse_file
from qapi import parse_schema, mcgen, camel_case, de_camel_case
from qapi_visit import generate_visit_struct, push_indent, pop_indent
import sys
import getopt
import os
import errno

def qapi_schema(node):
    schema = OrderedDict()
    data = OrderedDict()
    fields = None
    if node.has_key('typedef'):
        schema['type'] = node['typedef']
        fields = node['type']['fields']
    elif node.has_key('struct'):
        schema['type'] = node['struct']
        fields = node['fields']
    else:
        raise Exception("top-level neither typedef nor struct")

    for field in fields:
        if filter(lambda x: field.has_key(x), \
                ['is_derived', 'is_immutable', 'is_broken', 'is_function', \
                 'is_nested_decl', 'is_elsewhere']):
            continue

        description = OrderedDict()

        if field['type'].endswith('_t'):
            typename = field['type'][:-2]
        elif field['type'].startswith('struct '):
            typename = field['type'].split(" ")[1]
        elif field['type'].startswith('enum '):
            typename = 'int'
        elif field['type'] == "_Bool":
            typename = 'bool'
        elif field['type'].endswith("char") and field.has_key('is_pointer'):
            typename = 'str';
        elif field['type'] == 'int':
            typename = 'int32'
        elif field['type'] == 'unsigned int':
            typename = 'uint32'
        elif field['type'] == 'char':
            typename = 'uint8'
        else:
            typename = field['type']

        if field.has_key('is_array') and field['is_array']:
            description['type'] = [typename]
            description['<annotated>'] = 'true'
            if field.has_key('array_size'):
                description['array_size'] = field['array_size']
            if field.has_key('array_capacity'):
                description['array_capacity'] = field['array_capacity']
        elif camel_case(de_camel_case(typename)) == typename and \
            (not field.has_key('is_pointer') or not field['is_pointer']):
            description['type'] = typename
            description['<annotated>'] = 'true'
            description['embedded'] = 'true'
        else:
            description = typename

        if field.has_key('is_optional') and field['is_optional']:
            data["*" + field['variable']] = description
        else:
            data[field['variable']] = description

    schema['data'] = data
    return schema

def parse_schema_file(filename):
    return parse_schema(open(filename, 'r'))

def write_file(output, filename):
    if filename:
        output_file = open(filename, 'w')
    else:
        output_file = sys.stdout
    output_file.write(output)
    if filename:
        output_file.close()

def property_list(node):
    prop_list = []
    fields = None
    if node.has_key('typedef'):
        state = node['typedef']
        fields = node['type']['fields']
    elif node.has_key('struct'):
        state = node['struct']
        fields = node['fields']
    else:
        raise Exception("top-level neither typedef nor struct")

    for field in fields:
        if not field.has_key('is_property'):
            continue

        for arglist in field['property_fields']:
            if field['variable'] == 'devfn':
                typename = 'pci_devfn'
            elif field['type'].endswith('_t'):
                typename = field['type'][:-2]
            elif field['type'] == "_Bool":
                typename = 'bool'
            elif field.has_key('is_pointer'):
                if field['type'] in ("char", "const char"):
                    typename = "string"
                elif field['type'] == "void":
                    typename = "ptr"
            else:
                typename = field['type']

            prop = {}
            prop['name'] = arglist[0]
            prop['state'] = state
            prop['type'] = typename
            prop['field'] = field['variable']
            if len(arglist) == 2:
                prop['default'] = arglist[1]
            elif len(arglist) == 3:
                prop['type'] = 'bit'
                prop['bit'] = arglist[1]
                prop['default'] = arglist[2]

            prop_list.append(prop)

    return prop_list

def generate_include(include_path):
    return mcgen('''
#include "%(include)s"
''',
                       include=include_path)

def generate_property_bit(type_name, prop):
    if prop.has_key('default'):
        return mcgen('''
        DEFINE_PROP_BIT(%(name)s, %(state)s, %(field)s, %(bit)s, %(default)s),
''',
                        name=prop['name'], state=prop['state'],
                        field=prop['field'], bit=prop['bit'],
                        default=prop['default'])
    return mcgen('''
        DEFINE_PROP_BIT(%(name)s, %(state)s, %(field)s, %(bit)s),
''',
                 name=prop['name'], state=prop['state'],
                 field=prop['field'], bit=prop['bit'])

def generate_property(type_name, prop):
    if prop.has_key('default'):
        return mcgen('''
        DEFINE_PROP_%(type)s(%(name)s, %(state)s, %(field)s, %(default)s),
''',
                        type=prop['type'].upper(),  name=prop['name'],
                        state=prop['state'], field=prop['field'],
                        default=prop['default'])
    return mcgen('''
        DEFINE_PROP_%(type)s(%(name)s, %(state)s, %(field)s),
''',
                 type=prop['type'].upper(),  name=prop['name'],
                 state=prop['state'], field=prop['field'])

def generate_properties(type_name, prop_list=[]):
    output = ""

    for prop in prop_list:
        if prop['type'] == 'bit':
            output += generate_property_bit(type_name, prop)
        else:
            output += generate_property(type_name, prop)

    output += mcgen('''
        DEFINE_PROP_END_OF_LIST()
''')

    return output

def generate_qidl_registration(type_name, schema, do_state, prop_list=[]):
    schema_text = ""
    for line in schema.to_json().split("\n"):
        schema_text += "\n\"%s\\n\"" % line
    visitor = "NULL"
    if do_state:
        visitor = "visit_type_%s" % type_name

    return mcgen('''
static char *%(type_name)s_get_schema(Object *obj, Error **errp)
{
    return g_strdup(qidl_data_%(type_name)s.schema_json_text);
}

static void %(type_name)s_register_qidl(void)
{
    static Property properties[] = {
%(properties)s
    };
    ObjectProperty *schema_link;

    qidl_data_%(type_name)s.properties = properties;
    qidl_data_%(type_name)s.visitor = %(visitor)s;
    qidl_data_%(type_name)s.schema_json_text = %(schema_text)s;

    schema_link = object_property_find(container_get(object_get_root(), "/qidl/schemas"),
                                       "%(type_name)s", NULL);
    qidl_data_%(type_name)s.schema_obj = container_get(object_get_root(), "/qidl/schemas/%(type_name)s");
    if (!schema_link) {
        object_property_add_str(qidl_data_%(type_name)s.schema_obj, "json_text",
                                %(type_name)s_get_schema, NULL, NULL);
    }
}

qidl_init(%(type_name)s_register_qidl)
''',
                 type_name=type_name, schema_text=schema_text, visitor=visitor,
                 properties=generate_properties(type_name, prop_list))

def main(argv=[]):
    try:
        opts, args = getopt.gnu_getopt(argv[1:], "o:cd:I:",
                                       ["output-filepath=", "schema-filepath=",
                                        "include="])
    except getopt.GetoptError, err:
        print >> sys.stderr, err
        return 1

    output_filepath = None
    schema_filepath = None
    includes = ["hw/qdev-properties.h"]
    for o, a in opts:
        if o in ("-f", "--output-filepath"):
            output_filepath = a
        if o in ("-s", "--schema-filepath"):
            schema_filepath = a
        elif o in ("-I", "--include"):
            includes.append(a)

    nodes = parse_file(sys.stdin)
    if not nodes:
        return 2

    if os.path.dirname(output_filepath) != "":
        try:
            os.makedirs(os.path.dirname(output_filepath))
        except os.error, e:
            if e.errno != errno.EEXIST:
                raise
    output = ""
    schema_text_all = ""
    for include in includes:
        output += generate_include(include)
    for node in nodes:
        do_state = False
        schema = qapi_schema(node)
        prop_list = []
        # qapi parser expects iteration to be line-by-line
        schema_text = schema.to_json()
        expr = parse_schema(schema_text.split("\n"))[0]
        schema_text_all += schema_text + "\n\n"

        if node.has_key('do_state') and node['do_state']:
            do_state = True
            output += generate_visit_struct(expr['type'], expr['data'], True)
        if node.has_key('do_properties') and node['do_properties']:
            prop_list = property_list(node)

        output += generate_qidl_registration(expr['type'], schema, do_state, prop_list)

    if schema_filepath and schema_text_all != "":
        write_file(schema_text_all, schema_filepath)
    write_file(output, output_filepath)

    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))

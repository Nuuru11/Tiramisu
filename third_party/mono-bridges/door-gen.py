#!/usr/bin/env python3
"""Generate the mechanical FMOD Studio door wrappers (gen_studio_doors.inc)
from celeste.implmap (managed signatures — the ABI truth) and
bridge_fmod_studio.h (engine signatures).

Each wrapper is a SysV function with the exact FMOD_* native name that
forwards to the trt_fmod_studio_* engine function. Hand-written doors
(ParseID, SetParameterValuesByIndices, the core FMOD doors, the marker
exports) live in mono_fmod_door.cpp; this file emits only the mechanical
Studio wrappers.

Type mapping (managed -> C door param):
  native int -> void* ; unsigned int8[] -> unsigned char*
  int32 -> int ; unsigned int32 -> unsigned int ; int64 -> long long
  unsigned int64 -> unsigned long long ; float32 -> float
  float64 -> double ; bool -> int (4-byte marshaled FMOD_BOOL)
  [out] T& -> T* ; native int[] -> void**
  valuetype _3D_ATTRIBUTES -> trt_fmod_3d_attrs*
  valuetype PARAMETER_DESCRIPTION_INTERNAL -> param_desc_internal*
    (filled via a trt_fmod_param_desc temp + fill_param_desc_internal)
  valuetype System.Guid -> unsigned char* (16 bytes, always by-ref)
  class EVENT_CALLBACK -> trt_fmod_event_callback
  any other valuetype (FMOD enum) -> int
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parent.parent  # third_party/mono-bridges -> repo root

ALIASES = {
    'system_init': 'FMOD_Studio_System_Initialize',
    'system_get_low_level': 'FMOD_Studio_System_GetLowLevelSystem',
    'system_set_listener_attrs': 'FMOD_Studio_System_SetListenerAttributes',
    'system_get_listener_attrs': 'FMOD_Studio_System_GetListenerAttributes',
    'bank_get_id': 'FMOD_Studio_Bank_GetID',
    'bank_get_vca_count': 'FMOD_Studio_Bank_GetVCACount',
    'bank_get_vca_list': 'FMOD_Studio_Bank_GetVCAList',
    'bus_get_id': 'FMOD_Studio_Bus_GetID',
    'event_get_id': 'FMOD_Studio_EventDescription_GetID',
    'event_is_3d': 'FMOD_Studio_EventDescription_Is3D',
    'event_get_min_distance': 'FMOD_Studio_EventDescription_GetMinimumDistance',
    'event_get_max_distance': 'FMOD_Studio_EventDescription_GetMaximumDistance',
    'instance_get_3d_attrs': 'FMOD_Studio_EventInstance_Get3DAttributes',
    'instance_set_3d_attrs': 'FMOD_Studio_EventInstance_Set3DAttributes',
    'vca_get_id': 'FMOD_Studio_VCA_GetID',
    'system_is_valid': 'FMOD_Studio_System_IsValid',
    'bank_is_valid': 'FMOD_Studio_Bank_IsValid',
}

# Engine functions with no door (introspection, or hand-written in the door).
SKIP_ENGINE = {'total_event_count', 'total_sample_count',
               'total_pcm_bytes_submitted', 'bank_sample_count',
               'global_event_count'}
# FMOD names hand-written in mono_fmod_door.cpp (not generated).
SKIP_FMOD = {'FMOD_Studio_ParseID',
             'FMOD_Studio_EventInstance_SetParameterValuesByIndices'}


def cap(s):
    return ''.join(w.capitalize() for w in s.split('_'))


def eng_to_fmod(s):
    if s in ALIASES:
        return ALIASES[s]
    if s.startswith('system_'):
        return 'FMOD_Studio_System_' + cap(s[7:])
    if s == 'load_bank_file':
        return 'FMOD_Studio_System_LoadBankFile'
    if s == 'get_event_by_id':
        return 'FMOD_Studio_System_GetEventByID'
    if s == 'get_event':
        return 'FMOD_Studio_System_GetEvent'
    if s == 'get_bus_by_id':
        return 'FMOD_Studio_System_GetBusByID'
    if s == 'get_bus':
        return 'FMOD_Studio_System_GetBus'
    if s == 'get_vca_by_id':
        return 'FMOD_Studio_System_GetVCAByID'
    if s == 'get_vca':
        return 'FMOD_Studio_System_GetVCA'
    if s.startswith('bank_'):
        return 'FMOD_Studio_Bank_' + cap(s[5:])
    if s.startswith('event_'):
        return 'FMOD_Studio_EventDescription_' + cap(s[6:])
    if s.startswith('instance_'):
        r = s[9:]
        if r.startswith('get_parameter_value_by_index'):
            return 'FMOD_Studio_EventInstance_GetParameterValueByIndex'
        if r.startswith('set_parameter_value_by_index'):
            return 'FMOD_Studio_EventInstance_SetParameterValueByIndex'
        if r.startswith('get_parameter_value'):
            return 'FMOD_Studio_EventInstance_GetParameterValue'
        if r.startswith('set_parameter_value'):
            return 'FMOD_Studio_EventInstance_SetParameterValue'
        if r.startswith('get_parameter_by_index'):
            return 'FMOD_Studio_EventInstance_GetParameterByIndex'
        if r.startswith('get_parameter_count'):
            return 'FMOD_Studio_EventInstance_GetParameterCount'
        if r.startswith('get_parameter'):
            return 'FMOD_Studio_EventInstance_GetParameter'
        return 'FMOD_Studio_EventInstance_' + cap(r)
    if s.startswith('parameter_'):
        return 'FMOD_Studio_ParameterInstance_' + cap(s[10:])
    if s.startswith('bus_'):
        return 'FMOD_Studio_Bus_' + cap(s[4:])
    if s.startswith('vca_'):
        return 'FMOD_Studio_VCA_' + cap(s[4:])
    if s == 'lookup_id':
        return 'FMOD_Studio_System_LookupID'
    if s == 'lookup_path':
        return 'FMOD_Studio_System_LookupPath'
    return None


IMPLMAP_RE = re.compile(
    r"^\d+: (.*?) (?:class |valuetype )?[\w\.]+::\w+\((.*)\) \d+ \((\S+) (\S+)\)$"
)


def parse_implmap(path):
    """Return {native_name: (rettype, [argtype, ...])}."""
    entries = {}
    for line in Path(path).read_text().splitlines():
        m = IMPLMAP_RE.match(line.strip())
        if not m:
            continue
        rettype, args, name, module = m.groups()
        arglist = [a.strip() for a in args.split(',')] if args.strip() else []
        entries[name] = (rettype, arglist)
    return entries


def parse_engine_header(path):
    """Return {suffix: [param_c_type, ...]} for trt_fmod_studio_* fns."""
    text = Path(path).read_text()
    text = re.sub(r'//[^\n]*', '', text)  # strip line comments
    text = re.sub(r'\s+', ' ', text)      # flatten
    out = {}
    for m in re.finditer(r'int trt_fmod_studio_(\w+)\((.*?)\);', text):
        suffix, params = m.group(1), m.group(2).strip()
        if params in ('void', ''):
            out[suffix] = []
            continue
        plist = []
        for p in params.split(','):
            p = p.strip()
            # drop the parameter name (last token), keep the type
            p = re.sub(r'\[\d+\]$', '', p)  # u8 out_guid[16] -> u8 out_guid
            toks = p.split()
            plist.append(' '.join(toks[:-1]))
        out[suffix] = plist
    return out


def managed_to_c(arg):
    """Map one managed arg type to a C door parameter type."""
    out_ref = arg.startswith('[out] ')
    if out_ref:
        arg = arg[6:].rstrip('&').strip()
    if arg == 'native int':
        return 'void *' if not out_ref else 'void **'
    if arg == 'unsigned int8[]':
        return 'unsigned char *'
    if arg == 'native int[]':
        return 'void **'
    if arg == 'int32[]':
        return 'int *'
    if arg == 'float32[]':
        return 'float *'
    if arg == 'int32':
        return 'int' if not out_ref else 'int *'
    if arg == 'unsigned int32':
        return 'unsigned int' if not out_ref else 'unsigned int *'
    if arg == 'int64':
        return 'long long' if not out_ref else 'long long *'
    if arg == 'unsigned int64':
        return 'unsigned long long' if not out_ref else 'unsigned long long *'
    if arg == 'float32':
        return 'float' if not out_ref else 'float *'
    if arg == 'float64':
        return 'double' if not out_ref else 'double *'
    if arg == 'bool':
        return 'int' if not out_ref else 'int *'
    if arg == 'string':
        return 'const char *'
    if arg == 'valuetype FMOD.Studio._3D_ATTRIBUTES':
        return 'trt_fmod_3d_attrs *'
    if arg == 'valuetype FMOD.Studio.PARAMETER_DESCRIPTION_INTERNAL':
        return 'param_desc_internal *'
    if arg == 'valuetype [mscorlib]System.Guid':
        return 'unsigned char *'
    if arg == 'class FMOD.Studio.EVENT_CALLBACK':
        return 'trt_fmod_event_callback'
    if arg.startswith('valuetype ') or arg.startswith('class '):
        # FMOD enums / other valuetypes are int-sized.
        return 'int' if not out_ref else 'int *'
    raise SystemExit(f'UNKNOWN managed arg type: {arg!r}')


def engine_call_arg(ctype_door, ctype_eng, name):
    """Cast a door arg to the engine param type (or pass through)."""
    e = ctype_eng.replace(' ', '')
    d = ctype_door.replace(' ', '')
    if e == 'constchar*' and d == 'unsignedchar*':
        return f'(const char*){name}'
    if e == 'char*' and d == 'unsignedchar*':
        return f'(char*){name}'
    if e in ('u8*', 'u8') and d == 'unsignedchar*':
        return name  # u8 is unsigned char
    if e == 'consttrt_fmod_3d_attrs*' and d == 'trt_fmod_3d_attrs*':
        return name  # implicit const conversion
    if e == 'u32' and d == 'unsignedint':
        return name
    if e == 'u64' and d == 'unsignedlonglong':
        return name
    if e == 'i32' and d == 'int':
        return name
    return name


def main():
    impl = parse_implmap(HERE / 'celeste.implmap')
    eng = parse_engine_header(TREE / 'src/trt/bridge_fmod_studio.h')
    real = {ln.strip() for ln in (HERE / 'real-fmod.txt').read_text().splitlines()
            if ln.strip() and not ln.startswith('#')}

    lines = []
    lines.append('// GENERATED by door-gen.py from celeste.implmap + '
                 'bridge_fmod_studio.h — do not edit by hand.')
    lines.append('// Mechanical FMOD Studio door wrappers: each forwards to')
    lines.append('// the trt_fmod_studio_* engine function. Included by')
    lines.append('// mono_fmod_door.cpp inside extern "C".')
    lines.append('')
    n = 0
    for suffix in sorted(eng):
        if suffix in SKIP_ENGINE:
            continue
        fmod_name = eng_to_fmod(suffix)
        if fmod_name is None:
            raise SystemExit(f'NO FMOD NAME for engine fn {suffix}')
        if fmod_name in SKIP_FMOD:
            continue
        if fmod_name not in real:
            raise SystemExit(f'{fmod_name} (engine {suffix}) not in real-fmod.txt')
        if fmod_name not in impl:
            raise SystemExit(f'{fmod_name} not in celeste.implmap')
        rettype, args = impl[fmod_name]
        eng_params = eng[suffix]
        if len(args) != len(eng_params):
            raise SystemExit(
                f'ARITY MISMATCH {fmod_name}: implmap {len(args)} vs '
                f'engine {len(eng_params)}')
        # Door signature.
        c_ret = 'int'  # FMOD.RESULT and bool both marshal as int
        door_types = [managed_to_c(a) for a in args]
        params = ', '.join(f'{t} a{i}' for i, t in enumerate(door_types))
        if not params:
            params = 'void'
        lines.append(f'/* {fmod_name} <- trt_fmod_studio_{suffix} */')
        lines.append(f'MONO_DOOR_EXPORT {c_ret} {fmod_name}({params}) {{')
        # Param-desc out-params need a temp + fill.
        call_args = []
        pre = []
        post = []
        for i, (dt, et) in enumerate(zip(door_types, eng_params)):
            name = f'a{i}'
            if et == 'trt_fmod_param_desc*':
                pre.append('  trt_fmod_param_desc tmp;')
                call_args.append('&tmp')
                post.append(f'  if (rc == 0) fill_param_desc_internal({name}, &tmp);')
            else:
                call_args.append(engine_call_arg(dt, et, name))
        for p in pre:
            lines.append(p)
        call = f'trt_fmod_studio_{suffix}({", ".join(call_args)})'
        if post:
            lines.append(f'  int rc = {call};')
            lines.extend(post)
            lines.append('  return rc;')
        else:
            lines.append(f'  return {call};')
        lines.append('}')
        lines.append('')
        n += 1
    (HERE / 'gen').mkdir(exist_ok=True)
    (HERE / 'gen' / 'gen_studio_doors.inc').write_text('\n'.join(lines))
    print(f'door-gen: {n} Studio wrappers -> gen/gen_studio_doors.inc')


if __name__ == '__main__':
    main()

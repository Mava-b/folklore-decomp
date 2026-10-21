#!/usr/bin/env python3
"""Keep guest registers in C locals inside each lifted function.

The lifter writes every guest instruction as a statement on ctx->gpr[N],
ctx->cr, ... . Those are memory, and guest memory is reached through the
same untyped pointers, so after every guest store the compiler has to assume
ppu_context changed: each register is stored the moment it is written and
loaded again after the next store, and a compare + branch is a load,
read-modify-write and store of ctx->cr before the test.

This pass rewrites each func_XXXXXXXX body so the registers it touches live
in locals (r_N, f_N, p_cr, p_xer, p_ctr). ppu_context stays the
architectural state at every point where anything else can look at it:

  * before a call that takes ctx (func_*, ps3_indirect_call[_to],
    lv2_syscall, ps3_hle_call, DRAIN_TRAMPOLINE) every local that may have
    changed since the last such point is stored back ("dirty" set, from a
    forward dataflow over the function's labels and gotos);
  * after it, every promoted register is loaded again (dead loads are
    removed by the compiler). With --abi only the registers a callee may
    change under the PowerPC ABI are: r0, r2-r12, cr, xer, ctr, f0-f13;
  * before every return (and a fall-through off the end) the dirty set is
    stored (with --abi, only cr2-cr4 of the CR);
  * a forward edge into a loop header stores what the loop does not keep
    dirty, so registers written before a loop are not stored again at every
    call inside it.

A function is left untouched when it contains anything this pass does not
model: a multi-line block, ctx passed anywhere else, the address of a
promoted field taken other than &ctx->fpr[N] / &ctx->xer (memcpy and the
sraw/srad helpers, which only access that field), or a goto to a label it
does not define.

Usage: promote_regs.py [--abi] <in.cpp> <out.cpp>
       promote_regs.py [--abi] --dir <source dir> <output dir>
The --dir form converts every .cpp in the source folder and rewrites an
output file only when its source (or this script) is newer, so a rebuild
recompiles only what changed. A summary goes to stderr.
"""
import glob
import os
import re
import sys

FUNCTION_START = re.compile(r'^void (func_[0-9A-F]+)\(ppu_context\* ctx\) \{\s*$')
LABEL = re.compile(r'^(loc_[0-9A-F]+):\s*$')
GOTO_TARGET = re.compile(r'\bgoto (loc_[0-9A-F]+);')
COMMENT = re.compile(r'/\*.*?\*/')

SYNC_CALL_NAMES = r'(?:func_[0-9A-F]+|ps3_indirect_call_to|ps3_indirect_call|lv2_syscall|ps3_hle_call|DRAIN_TRAMPOLINE)'
# A sync call statement, optionally followed by DRAIN_TRAMPOLINE(ctx); which
# is treated as part of the same call.
SYNC_CALL = re.compile(r'\b(' + SYNC_CALL_NAMES + r')\(([^;()]*)\);(\s*DRAIN_TRAMPOLINE\(ctx\);)?')
# Bare uses of ctx that this pass understands without a sync.
FPSCR_HELPER = re.compile(r'\b(?:ppu_mtfsb|ppu_mtfsf|ppu_mcrfs)\(ctx,')

FIELD_REF = re.compile(r'ctx->(gpr|fpr)\[(\d+)\]|ctx->(cr|xer|ctr)\b')

VOLATILE_GPRS = set([0] + list(range(2, 13)))
# --abi: trust the PowerPC ABI -- reload only the volatile registers after a
# call and keep only cr2-cr4 at a return. The default reloads everything a
# function promoted after every call and stores the whole CR at returns,
# which is correct even for guest code that does not follow the ABI.
ABI_TRUST = '--abi' in sys.argv
VOLATILE_FPRS = set(range(0, 14))


class Field:
    def __init__(self, kind, index):
        self.kind = kind
        self.index = index
        if kind == 'gpr':
            self.local = 'r_%d' % index
            self.ctype = 'uint64_t'
            self.memory = 'ctx->gpr[%d]' % index
            self.volatile = index in VOLATILE_GPRS
        elif kind == 'fpr':
            self.local = 'f_%d' % index
            self.ctype = 'double'
            self.memory = 'ctx->fpr[%d]' % index
            self.volatile = index in VOLATILE_FPRS
        else:
            self.local = 'p_' + kind
            self.ctype = 'uint64_t' if kind == 'ctr' else 'uint32_t'
            self.memory = 'ctx->' + kind
            self.volatile = True


def strip_comments(line):
    return COMMENT.sub('', line)


def unsupported_reason(body):
    for line in body:
        code = strip_comments(line).strip()
        if code.endswith('{') or code == '}' or code.startswith('} else'):
            return 'multi-line block'
        if '&ctx->gpr' in code or '&ctx->cr' in code or '&ctx->ctr' in code:
            return 'address of a promoted field'
        if 'ppu_context' in code:
            return 'ppu_context named in body'
        rest = SYNC_CALL.sub('', code)
        rest = FPSCR_HELPER.sub('', rest)
        rest = re.sub(r'ctx->', '', rest)
        if re.search(r'\bctx\b', rest):
            return 'ctx used directly'
    return None


def assigned_locals(code, local_names):
    """Locals a statement sequence may write (conservative)."""
    written = set()
    for match in re.finditer(r'\b(r_\d+|f_\d+|p_cr|p_xer|p_ctr)\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)', code):
        written.add(match.group(1))
    for match in re.finditer(r'(?:\+\+|--)\s*(r_\d+|f_\d+|p_cr|p_xer|p_ctr)\b', code):
        written.add(match.group(1))
    for match in re.finditer(r'\b(r_\d+|f_\d+|p_cr|p_xer|p_ctr)\s*(?:\+\+|--)', code):
        written.add(match.group(1))
    for match in re.finditer(r'&(r_\d+|f_\d+|p_cr|p_xer|p_ctr)\b', code):
        written.add(match.group(1))
    return written & local_names


def is_top_level(prefix):
    """True when text before a statement leaves it unconditional."""
    code = prefix
    if re.search(r'\b(if|switch|case|default|else|for|while)\b', code):
        return False
    return code.count('{') == code.count('}') and code.count('(') == code.count(')')


def promote_function(header, body):
    reason = unsupported_reason(body)
    if reason:
        return None, reason

    labels = {}
    for index, line in enumerate(body):
        match = LABEL.match(line.strip())
        if match:
            labels[match.group(1)] = index

    used = {}
    for line in body:
        for match in FIELD_REF.finditer(strip_comments(line)):
            if match.group(1):
                key = (match.group(1), int(match.group(2)))
            else:
                key = (match.group(3), 0)
            if key not in used:
                used[key] = Field(key[0], key[1])
    if not used:
        return None, 'no registers'
    fields = sorted(used.values(), key=lambda field: ({'gpr': 0, 'fpr': 1, 'cr': 2, 'xer': 3, 'ctr': 4}[field.kind], field.index))
    by_local = dict((field.local, field) for field in fields)
    local_names = set(by_local)
    bit_of = dict((field.local, 1 << position) for position, field in enumerate(fields))

    def mask(names):
        value = 0
        for name in names:
            value |= bit_of[name]
        return value

    def replace_fields(line):
        def substitute(match):
            if match.group(1):
                return used[(match.group(1), int(match.group(2)))].local
            return used[(match.group(3), 0)].local
        parts = re.split(r'(/\*.*?\*/)', line)
        return ''.join(part if part.startswith('/*') else FIELD_REF.sub(substitute, part) for part in parts)

    replaced = [replace_fields(line) for line in body]

    # Per line: writes, whether it ends a fall-through, goto targets, and
    # the writes after its last unconditional sync call (if any).
    count = len(replaced)
    line_writes = []
    line_clean_writes = []      # None when the line has no unconditional sync
    successors = []
    for index, line in enumerate(replaced):
        code = strip_comments(line)
        writes = mask(assigned_locals(code, local_names))
        line_writes.append(writes)
        clean = None
        last_end = None
        for match in SYNC_CALL.finditer(code):
            if is_top_level(code[:match.start()]):
                last_end = match.end()
        if last_end is not None:
            clean = mask(assigned_locals(code[last_end:], local_names))
        line_clean_writes.append(clean)
        targets = []
        for target in GOTO_TARGET.findall(code):
            if target not in labels:
                return None, 'goto to unknown label %s' % target
            targets.append(labels[target])
        stripped = code.strip()
        if not (stripped.startswith('goto ') or stripped.startswith('return;')) and index + 1 < count:
            targets.append(index + 1)
        successors.append(targets)

    # Loop headers: labels reached by a backward edge. Without care, a
    # register written before a loop stays "dirty" around the loop (the
    # entry edge and the back edge merge at the header) and is stored again
    # at every call in every iteration. So a header gets a fixed dirty set
    # D -- what can still be dirty when the loop comes round -- and each
    # forward edge into it stores whatever else it carries, once.
    predecessors = [[] for _ in range(count)]
    for index, targets in enumerate(successors):
        for target in targets:
            predecessors[target].append(index)
    headers = set(label for label in labels.values()
                  if any(pred >= label for pred in predecessors[label]))
    header_dirty = dict((label, 0) for label in headers)

    def solve():
        dirty_in = [0] * count
        dirty_out = [None] * count
        changed = True
        while changed:
            changed = False
            for index in range(count):
                if index == 0:
                    reached = True
                    incoming = 0
                else:
                    reached_preds = [pred for pred in predecessors[index] if dirty_out[pred] is not None]
                    reached = bool(reached_preds)
                    incoming = 0
                    for pred in reached_preds:
                        incoming |= dirty_out[pred]
                if not reached:
                    continue
                if index in headers:
                    incoming = header_dirty[index]
                if line_clean_writes[index] is not None:
                    out = line_clean_writes[index]
                else:
                    out = incoming | line_writes[index]
                if dirty_in[index] != incoming or dirty_out[index] != out:
                    dirty_in[index] = incoming
                    dirty_out[index] = out
                    changed = True
        return dirty_in, dirty_out

    while True:
        dirty_in, dirty_out = solve()
        grown = False
        for label in headers:
            around = 0
            for pred in predecessors[label]:
                if pred >= label and dirty_out[pred] is not None:
                    around |= dirty_out[pred]
            if around & ~header_dirty[label]:
                header_dirty[label] |= around
                grown = True
        if not grown:
            break

    def store_list(dirty, at_return=False):
        statements = []
        for field in fields:
            if not dirty & bit_of[field.local]:
                continue
            if at_return and field.kind == 'cr' and ABI_TRUST:
                # Only cr2-cr4 survive a return (the ABI makes cr0, cr1 and
                # cr5-cr7 volatile, so no caller reads them afterwards).
                # Leaving the volatile fields out of the final store lets the
                # compiler drop every compare whose only other use is a
                # branch -- most of them.
                statements.append('ctx->cr = (ctx->cr & ~0x00FFF000u) | (p_cr & 0x00FFF000u);')
                continue
            statements.append('%s = %s;' % (field.memory, field.local))
        return ' '.join(statements)

    reload_after_call = ' '.join('%s = %s;' % (field.local, field.memory) for field in fields if field.volatile or not ABI_TRUST)

    output = [header]
    output.append('        ' + ' '.join('%s %s = %s;' % (field.ctype, field.local, field.memory) for field in fields))
    for index, line in enumerate(replaced):
        if dirty_out[index] is None:
            output.append(line)     # unreachable
            continue
        # Everything that may be dirty anywhere on this line: stores of a
        # local that has not changed are harmless, so one set serves every
        # sync point and return on the line.
        line_dirty = dirty_in[index] | line_writes[index]
        stores = store_list(line_dirty)

        def sync(match):
            call = match.group(0)
            return '{ %s %s %s }' % (stores, call, reload_after_call)
        new_line = SYNC_CALL.sub(sync, line)
        # Forward gotos into a loop header store what the header does not
        # expect to be dirty.
        def edge_goto(match):
            target = labels[match.group(1)]
            if target in headers and target > index:
                edge_stores = store_list(line_dirty & ~header_dirty[target])
                if edge_stores:
                    return '{ %s goto %s; }' % (edge_stores, match.group(1))
            return match.group(0)
        new_line = GOTO_TARGET.sub(edge_goto, new_line)
        if re.search(r'\breturn;', strip_comments(new_line)):
            # A return that hands over to a trampoline continues this same
            # guest code in another fragment, which may still branch on a
            # volatile CR field: keep the whole CR there.
            trampoline = 'g_trampoline_fn' in new_line
            return_stores = stores if trampoline else store_list(line_dirty, at_return=True)
            new_line = re.sub(r'\breturn;', '{ %s return; }' % return_stores if return_stores else 'return;', new_line)
        # Falling through into a loop header is a forward edge too.
        if index + 1 in headers and index + 1 in successors[index] and dirty_out[index] is not None:
            fall_stores = store_list(dirty_out[index] & ~header_dirty[index + 1])
            if fall_stores:
                new_line = new_line + '\n        ' + fall_stores
        output.append(new_line)
    # Falling off the end of the function is a return.
    last_code = strip_comments(replaced[-1]).strip() if replaced else ''
    if replaced and not (last_code.startswith('goto ') or last_code.startswith('return;')) and dirty_out[-1] is not None:
        final_stores = store_list(dirty_out[-1], at_return=True)
        if final_stores:
            output.append('        ' + final_stores)
    return output, None


def convert_file(source_path, output_path):
    lines = open(source_path, newline='').read().split('\n')
    output = []
    promoted = 0
    skipped = {}
    index = 0
    while index < len(lines):
        line = lines[index]
        start = FUNCTION_START.match(line.rstrip('\r'))
        if not start:
            output.append(line)
            index += 1
            continue
        end = index + 1
        while end < len(lines) and not lines[end].rstrip('\r') == '}':
            end += 1
        crlf = line.endswith('\r')
        header = line.rstrip('\r')
        body = [entry.rstrip('\r') for entry in lines[index + 1:end]]
        result, reason = promote_function(header, body)
        if result is None:
            skipped[reason] = skipped.get(reason, 0) + 1
            output.extend(lines[index:end + 1])
        else:
            promoted += 1
            output.extend(entry + ('\r' if crlf else '') for entry in result)
            output.append(lines[end])
        index = end + 1
    open(output_path, 'w', newline='').write('\n'.join(output))
    summary = ', '.join('%s: %d' % (reason, number) for reason, number in sorted(skipped.items(), key=lambda item: -item[1]))
    sys.stderr.write('%s: %d promoted; skipped %s\n' % (source_path, promoted, summary or 'none'))


def convert_directory(source_dir, output_dir):
    os.makedirs(output_dir, exist_ok=True)
    script_time = os.path.getmtime(os.path.abspath(__file__))
    wanted = set()
    for source_path in sorted(glob.glob(os.path.join(source_dir, '*.cpp'))):
        name = os.path.basename(source_path)
        wanted.add(name)
        output_path = os.path.join(output_dir, name)
        if os.path.exists(output_path):
            output_time = os.path.getmtime(output_path)
            if output_time >= os.path.getmtime(source_path) and output_time >= script_time:
                continue
        convert_file(source_path, output_path)
    for stale_path in glob.glob(os.path.join(output_dir, '*.cpp')):
        if os.path.basename(stale_path) not in wanted:
            os.remove(stale_path)


def main():
    arguments = [argument for argument in sys.argv[1:] if not argument.startswith('--')]
    if '--dir' in sys.argv:
        convert_directory(arguments[0], arguments[1])
    else:
        convert_file(arguments[0], arguments[1])


if __name__ == '__main__':
    main()

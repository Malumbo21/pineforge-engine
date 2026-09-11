"""Generate exact per-field reflection/hash mutation fixtures; no engine execution."""
from pathlib import Path
import sys
from exit_leg_reflection_schema import fields
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/fixtures/exit_lifecycle/reflection_mutations.inc'
VARIANTS={None:0,'BindOwner':0,'Suspend':1,'StageReplacement':2,'Restore':4,'CompleteBarrier':5,'Observe':6,'Cancel':7}
def generate():
    return '// Generated canonical field census; storage-only fixtures, not production histories.\n'+''.join(
        f'mutate("legs_{f.name}", {VARIANTS[f.variant]}, [](Lifecycle& state) {{ {f.mutation} }});\n' for f in fields())
if __name__=='__main__':
    value=generate()
    if '--check' in sys.argv:
        if not OUT.exists() or OUT.read_text()!=value:raise SystemExit('canonical mutation fixture out of date')
        print('canonical mutation fixture up to date')
    else:OUT.write_text(value)

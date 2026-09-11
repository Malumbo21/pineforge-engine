"""Bounded named POD projection of canonical exit lifecycle state.

Every row describes an actual scalar plus its optional/variant validity. The
mutation expression is a storage-census fixture, not a production action.
"""
from dataclasses import dataclass

@dataclass(frozen=True)
class Field:
    name: str
    kind: str
    read: str
    guard: str
    mutation: str
    variant: str | None = None
    @property
    def expression(self):
        inactive = 'std::numeric_limits<double>::quiet_NaN()' if self.kind == 'double' else 'UINT32_MAX' if self.name.endswith(('_item0','_item1','_item2')) else '0'
        return f'({self.guard}) ? ({self.read}) : ({inactive})'

def fields():
    out=[]
    def leaf(n,k,r,m,g='true',v=None):out.append(Field(n,k,r,g,m,v))
    def scalar(n,k,r,m,g,v):leaf(n,k,r,f'++({m});',g,v)
    def frame(n,r,m,g,v):
        scalar(n+'_event','uint64_t',f'({r}).event',f'({m}).event',g,v)
        scalar(n+'_bar','int64_t',f'({r}).bar',f'({m}).bar',g,v)
        leaf(n+'_domain','uint32_t',f'static_cast<uint32_t>(({r}).domain)',f'({m}).domain = static_cast<Domain>((static_cast<unsigned>(({m}).domain)+1)%5);',g,v)
        leaf(n+'_phase','uint32_t',f'static_cast<uint32_t>(({r}).phase)',f'({m}).phase = ({m}).phase == Phase::Observation ? Phase::AfterMargin : Phase::Observation;',g,v)
    def target(n,r,m,g,v):
        scalar(n+'_incarnation','uint64_t',f'({r}).incarnation',f'({m}).incarnation',g,v)
        scalar(n+'_owner','int64_t',f'({r}).owner',f'({m}).owner',g,v)
    def definition(n,r,m,g,v):
        leaf(n+'_incarnation','uint64_t',f'({r}).incarnation()',f'++definition_incarnation({m});',g,v)
        leaf(n+'_revision','uint64_t',f'({r}).revision()',f'++definition_revision({m});',g,v)
        leaf(n+'_value_present','uint8_t',f'({r}).has_value() ? 1 : 0',f'definition_value({m}).reset();',g,v)
        for key in 'limit_price stop_price trail_points trail_price trail_offset profit_ticks loss_ticks'.split():
            leaf(n+'_'+key,'double',f'({r}).prices().{key}',f'change_price({m}, &Prices::{key});',f'({g}) && ({r}).has_value()',v)
    def optional(n,fn,r,m,g,v):
        leaf(n+'_present','uint8_t',f'({r}).has_value() ? 1 : 0',f'({m}).reset();',g,v)
        fn(n,f'(*({r}))',f'(*({m}))',f'({g}) && ({r}).has_value()',v)
    def legs(n,r,m,g,v):
        leaf(n+'_count','uint32_t',f'static_cast<uint32_t>(({r}).size())',f'({m}).pop_back();',g,v)
        for i in range(3):
            leaf(n+f'_item{i}','uint32_t',f'static_cast<uint32_t>(({r})[{i}])',f'std::swap(({m})[{i}], ({m})[{(i+1)%3}]);',f'({g}) && ({r}).size() > {i}',v)
    def barrier(n,r,m,g,v):
        frame(n+'_requested',f'({r}).requested',f'({m}).requested',g,v)
        target(n+'_target',f'({r}).target',f'({m}).target',g,v)
        scalar(n+'_revision','uint64_t',f'({r}).revision',f'({m}).revision',g,v)
    def retirement(n,r,m,g,v):
        scalar(n+'_generation','uint64_t',f'({r}).generation',f'({m}).generation',g,v)
        frame(n+'_cause',f'({r}).cause',f'({m}).cause',g,v)
    def window(n,r,m,g,v):
        frame(n+'_excluded',f'({r}).excluded',f'({m}).excluded',g,v)
        for key in ['best','prefix']:
            leaf(n+'_'+key,'double',f'({r}).{key}',f'({m}).{key} += 1;',g,v)
    def replacement(n,r,m,g,v):
        scalar(n+'_queue_predecessor','uint64_t',f'({r}).queue_predecessor',f'({m}).queue_predecessor',g,v)
        definition(n+'_revival_definition',f'({r}).revival_definition',f'({m}).revival_definition',g,v)
        barrier(n+'_release',f'({r}).release',f'({m}).release',g,v)
    def suspension(n,r,m,g,v):
        frame(n+'_cause',f'({r}).cause',f'({m}).cause',g,v)
        legs(n+'_legs',f'({r}).legs',f'({m}).legs',g,v)
        optional(n+'_hold',barrier,f'({r}).hold',f'({m}).hold',g,v)
        optional(n+'_revival_definition',definition,f'({r}).revival_definition',f'({m}).revival_definition',g,v)
        optional(n+'_replacement',replacement,f'({r}).replacement',f'({m}).replacement',g,v)
        optional(n+'_window',window,f'({r}).window',f'({m}).window',g,v)
    def action(n,r,m,g,v):
        target(n+'_target',f'({r}).target',f'({m}).target',g,v)
        scalar(n+'_expected_revision','uint64_t',f'({r}).expected_revision',f'({m}).expected_revision',g,v)
        frame(n+'_cause',f'({r}).cause',f'({m}).cause',g,v)
        leaf(n+'_operation','uint32_t',f'static_cast<uint32_t>(({r}).operation.index())',f'({m}).operation = CancelDeferredActivation{{}};',g,v)
        for op in ['BindOwner','Suspend','StageReplacement','Restore','CompleteBarrier','Observe','Cancel']:
            read=f'std::get<exit_legs::{op}>(({r}).operation)'
            mut=f'std::get<{op}>(({m}).operation)'
            guard=f'({g}) && std::holds_alternative<exit_legs::{op}>(({r}).operation)'
            p=n+'_'+{'BindOwner':'bind','Suspend':'suspend','StageReplacement':'stage','Restore':'restore','CompleteBarrier':'complete','Observe':'observe','Cancel':'cancel'}[op]
            if op=='BindOwner':scalar(p+'_owner','int64_t',f'({read}).owner',f'({mut}).owner',guard,op)
            elif op=='Suspend':
                legs(p+'_legs',f'({read}).legs',f'({mut}).legs',guard,op)
                optional(p+'_hold',barrier,f'({read}).hold',f'({mut}).hold',guard,op)
                optional(p+'_window',window,f'({read}).window',f'({mut}).window',guard,op)
                legs(p+'_retire',f'({read}).retire',f'({mut}).retire',guard,op)
            elif op=='StageReplacement':replacement(p,f'({read}).relation',f'({mut}).relation',guard,op)
            elif op in ['Restore','Cancel']:legs(p+'_legs',f'({read}).legs',f'({mut}).legs',guard,op)
            elif op=='CompleteBarrier':
                frame(p+'_completed',f'({read}).completed',f'({mut}).completed',guard,op)
                optional(p+'_requested',barrier,f'({read}).requested',f'({mut}).requested',guard,op)
            elif op=='Observe':
                for key in ['high','low']:leaf(p+'_'+key,'double',f'({read}).{key}',f'({mut}).{key} += 1;',guard,op)
                scalar(p+'_direction','int32_t',f'({read}).direction',f'({mut}).direction',guard,op)
                leaf(p+'_fold','uint32_t',f'static_cast<uint32_t>(({read}).fold)',f'({mut}).fold = ({mut}).fold == Fold::Prefix ? Fold::Continue : Fold::Prefix;',guard,op)
    target('target','src.{m}.target()','target(state)','true',None)
    leaf('revision','uint64_t','src.{m}.revision()','++revision(state);')
    definition('definition','src.{m}.current_definition()','definition(state)','true',None)
    for i in range(3):
        scalar(f'generation{i}','uint64_t',f'src.{{m}}.generation(static_cast<exit_legs::Leg>({i}))',f'generations(state)[{i}]','true',None)
        optional(f'retirement{i}',retirement,f'src.{{m}}.retirements()[{i}]',f'retirements(state)[{i}]','true',None)
    optional('suspension',suspension,'src.{m}.suspension()','suspension(state)','true',None)
    optional('last',action,'src.{m}.last_action()','last(state)','true',None)
    assert len({f.name for f in out})==len(out)
    return out

def mapping():return [(f.name,f.kind,f.expression) for f in fields()]
def validate(actual):
    expected=mapping()
    if not actual or actual!=expected:
        missing=[n for n,_,_ in expected if n not in {x[0] for x in actual}]
        raise ValueError('incomplete canonical exit lifecycle reflection: '+str(missing))

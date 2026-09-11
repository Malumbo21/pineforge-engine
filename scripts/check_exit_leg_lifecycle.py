"""Fail-closed source reflection for the canonical native lifecycle (no compiler)."""
import re
from gen_pending_order_mirror import struct_body

def clean(s):
    return re.sub(r'//[^\n]*|/\*.*?\*/', '', s, flags=re.S)
def compact(s): return re.sub(r'\s+', '', s)
def declarations(body):
    body=re.sub(r"\b(?:public|private|protected):|friend\s+class\s+\w+;", "", body)
    # Storage only at top level; method bodies do not contribute members.
    result=[]; start=0; i=0
    while i<len(body):
        if body[i]=='{':
            prefix=body[start:i]
            depth=1;j=i+1
            while j<len(body) and depth:
                depth += (body[j]=='{')-(body[j]=='}');j+=1
            if depth: raise ValueError('unbalanced lifecycle declaration')
            if '(' in prefix and '=' not in prefix:
                start=j
            i=j;continue
        if body[i]==';':
            item=body[start:i].strip();start=i+1
            if item:result.append(compact(item))
        i+=1
    return result
def class_storage(source):
    """Census the entire canonical class, including fields after its helpers.

    Only the two known transient helper type declarations are skipped. An
    instance after a helper declaration is storage and fails closed.
    """
    marker=re.search(r'\bclass\s+Lifecycle\s*\{',source)
    if not marker: raise ValueError('missing canonical lifecycle class')
    start=marker.end();depth=1;end=start
    while end<len(source) and depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    if depth: raise ValueError('unbalanced canonical lifecycle class')
    body=source[start:end-1]
    macro=re.search(r'^\s*#define PF_LEG_PRICE_SETTER\(name\)\s*\\\n([^\n]+)',body,re.M)
    expected_setter='double set_##name(double value) { auto next = prices(); next.name = value; set_prices(std::move(next)); return value; }'
    if not macro or compact(macro[1])!=compact(expected_setter):
        raise ValueError('unreflected lifecycle setter macro storage')
    directives=re.findall(r'^\s*#(\w+)[ \t]+([^\n]+)',body,re.M)
    if len(directives)!=2 or directives[0][0]!='define' or directives[1]!=('undef','PF_LEG_PRICE_SETTER'):
        raise ValueError('unexpected lifecycle preprocessing')
    calls=re.findall(r'^\s*PF_LEG_PRICE_SETTER\((\w+)\)\s*$',body,re.M)
    if calls!='limit_price stop_price trail_points trail_price trail_offset profit_ticks loss_ticks'.split():
        raise ValueError('unreflected lifecycle price setter list')
    lines=[];continuation=False
    for line in body.splitlines():
        if continuation or line.lstrip().startswith('#'):
            continuation=line.rstrip().endswith('\\');continue
        if re.fullmatch(r'\s*PF_LEG_PRICE_SETTER\(\w+\)\s*',line):continue
        lines.append(line)
    body=re.sub(r'\b(?:public|private|protected):','', '\n'.join(lines))
    result=[];start=0;i=0
    while i<len(body):
        if body[i]=='{':
            prefix=body[start:i].strip();depth=1;j=i+1
            while j<len(body) and depth:
                depth+=(body[j]=='{')-(body[j]=='}');j+=1
            if depth:raise ValueError('unbalanced lifecycle member')
            nested=re.search(r'\bstruct\s+(\w+)\s*$',prefix)
            if nested:
                after=body.find(';',j)
                if nested[1] not in {'ReplaySink','Exact'} or after<0 or body[j:after].strip():
                    raise ValueError('unreflected nested lifecycle storage')
                start=after+1;i=after+1;continue
            # Parameter defaults are within parentheses; a top-level '='
            # distinguishes a data initializer/lambda from a member method.
            parens=0;assignment=False
            for ch in prefix:
                if ch=='(':parens+=1
                elif ch==')':parens-=1
                elif ch=='=' and parens==0:assignment=True
            if '(' in prefix and not assignment:start=j
            i=j;continue
        if body[i]==';':
            item=body[start:i].strip();start=i+1
            if item:result.append(compact(item))
        i+=1
    return result

SCHEMA={
 'Frame':['uint64_t event = 0','int64_t bar = -1','Domain domain = Domain::Ordinary','Phase phase = Phase::Observation'],
 'Target':['uint64_t incarnation = 0','int64_t owner = 0'],
 'Prices':[f'double {n} = absent()' for n in 'limit_price stop_price trail_points trail_price trail_offset profit_ticks loss_ticks'.split()],
 'Definition':['uint64_t incarnation_ = 0','uint64_t revision_ = 0','std::shared_ptr<const Prices> value_'],
 'Barrier':['Frame requested','Target target','uint64_t revision = 0'],
 'ObservationWindow':['Frame excluded','double best = absent()','double prefix = absent()'],
 'Retirement':['uint64_t generation','Frame cause'],
 'Replacement':['uint64_t queue_predecessor','Definition revival_definition','Barrier release'],
 'Suspension':['Frame cause','std::vector<Leg> legs','std::optional<Barrier> hold','std::optional<Definition> revival_definition','std::optional<Replacement> replacement','std::optional<ObservationWindow> window'],
 'BindOwner':['int64_t owner'],
 'Suspend':['std::vector<Leg> legs','std::optional<Barrier> hold','std::optional<ObservationWindow> window','std::vector<Leg> retire'],
 'StageReplacement':['Replacement relation'], 'CancelDeferredActivation':[],
 'Restore':['std::vector<Leg> legs'],'CompleteBarrier':['Frame completed','std::optional<Barrier> requested'],
 'Observe':['double high','double low','int direction','Fold fold'],'Cancel':['std::vector<Leg> legs'],
 'Action':['Target target','uint64_t expected_revision','Frame cause','Operation operation'],
}
PRIVATE=['Definition definition_','Target target_','uint64_t revision_ = 0','std::array<uint64_t, 3> generations_{{1, 1, 1}}','std::array<std::optional<Retirement>, 3> retired_','std::optional<Suspension> suspension_','std::optional<Action> last_']
FOLDS={
 'visit':['f.u(target_.incarnation);','f.i(target_.owner);','f.u(revision_);','visit_definition(f, definition_);','for (auto generation : generations_) f.u(generation);','for (const auto& retirement : retired_)','f.b(retirement.has_value());','f.u(retirement->generation);','visit_frame(f, retirement->cause);','f.b(suspension_.has_value());','visit_frame(f, suspension_->cause);','visit_legs(f, suspension_->legs);','visit_barrier(f, suspension_->hold);','f.b(suspension_->revival_definition.has_value());','visit_definition(f, *suspension_->revival_definition);','f.b(suspension_->replacement.has_value());','visit_replacement(f, *suspension_->replacement);','visit_window(f, suspension_->window);','f.b(last_.has_value());','visit_action(f, *last_);'],
 'visit_frame':['f.u(v.event);','f.i(v.bar);','f.u(static_cast<uint64_t>(v.domain));','f.u(static_cast<uint64_t>(v.phase));'],
 'visit_prices':[f'f.d(p.{n});' for n in 'limit_price stop_price trail_points trail_price trail_offset profit_ticks loss_ticks'.split()],
 'visit_definition':['f.u(d.incarnation_);','f.u(d.revision_);','f.b(d.value_ != nullptr);','visit_prices(f, d.prices());'],
 'visit_legs':['f.u(legs.size());','for (Leg leg : legs) f.u(static_cast<uint64_t>(leg));'],
 'visit_barrier_value':['visit_frame(f, b.requested);','f.u(b.target.incarnation);','f.i(b.target.owner);','f.u(b.revision);'],
 'visit_barrier':['f.b(b.has_value());','visit_barrier_value(f, *b);'],
 'visit_window':['f.b(w.has_value());','visit_frame(f, w->excluded);','f.d(w->best);','f.d(w->prefix);'],
 'visit_replacement':['f.u(r.queue_predecessor);','visit_definition(f, r.revival_definition);','visit_barrier_value(f, r.release);'],
 'visit_action':['ReplaySink<S> raw{f};','visit_action_fields(raw, a);'],
 'visit_action_fields':['f.u(a.target.incarnation);','f.i(a.target.owner);','f.u(a.expected_revision);','visit_frame(f, a.cause);','f.u(a.operation.index());','f.i(op.owner);','visit_legs(f, op.legs);','visit_barrier(f, op.hold);','visit_window(f, op.window);','visit_legs(f, op.retire);','visit_replacement(f, op.relation);','visit_frame(f, op.completed);','visit_barrier(f, op.requested);','f.d(op.high);','f.d(op.low);','f.i(op.direction);','f.u(static_cast<uint64_t>(op.fold));'],
}
def function_body(s,name):
    m=re.search(r'\bvoid\s+'+name+r'\([^)]*\)[^{]*\{',s)
    if not m: raise ValueError('missing lifecycle visitor '+name)
    i=m.end();depth=1;j=i
    while j<len(s) and depth:
        depth+=(s[j]=='{')-(s[j]=='}');j+=1
    if depth: raise ValueError('unbalanced visitor '+name)
    return s[i:j-1]
def check(header,engine_hash=None):
    from gen_pending_order_mirror import COMPOSITE_MAP
    from exit_leg_reflection_schema import validate
    validate(COMPOSITE_MAP["ExitLegLifecycle"])
    s=clean(header)
    for name,expected in SCHEMA.items():
        actual=declarations(struct_body(s,name))
        if actual!=list(map(compact,expected)):raise ValueError('unreflected lifecycle storage '+name+': '+str(actual))
    if class_storage(s)!=list(map(compact,PRIVATE)):raise ValueError('unreflected lifecycle class storage')
    for declaration in [
        'enum class Leg : uint8_t { Stop, Limit, Trail };',
        'enum class Domain : uint8_t { Ordinary, Coof, Magnifier, MagnifierCoof, RawTicks };',
        'enum class Phase : uint8_t { Observation, AfterMargin };',
        'enum class Fold : uint8_t { Prefix, Continue };',
    ]:
        if compact(declaration) not in compact(s): raise ValueError('unreflected lifecycle discriminant')
    expected='using Operation = std::variant<BindOwner, Suspend, StageReplacement, CancelDeferredActivation, Restore, CompleteBarrier, Observe, Cancel>;'
    if compact(expected) not in compact(s):raise ValueError('unreflected lifecycle operation variants')
    if compact("void d(double v) { uint64_t bits; std::memcpy(&bits, &v, sizeof(bits)); sink.u(bits); }") not in compact(s):
        raise ValueError("replay payload requires raw binary64 hashing")
    for name,folds in FOLDS.items():
        body=compact(function_body(s,name))
        for fold in folds:
            if compact(fold) not in body:raise ValueError('unreflected lifecycle '+name+': '+fold)
    if engine_hash is not None:
        from check_broker_state_hash_coverage import _collection_loop_body
        loop=compact(_collection_loop_body(clean(engine_hash),'pending_orders_','o'))
        if loop.count('o.legs.visit(f);')!=1:raise ValueError('canonical lifecycle must be hashed once in pending loop')

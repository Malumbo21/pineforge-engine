#!/usr/bin/env python3
"""Metadata mutations; never runs an engine or strategy."""
from pathlib import Path
import json
import tempfile
import unittest
import check_market_admission_schema as checker

ROOT=Path(__file__).resolve().parents[1]
FILES=['include/pineforge/market_admission.hpp','src/market_admission.cpp','src/engine_state_hash.cpp',
       'scripts/market_admission_schema.json','scripts/market_admission_mirror_fields.json',
       'scripts/broker_state_hash_waivers.txt','scripts/pending_order_mirror_waivers.txt']
DATA={name:(ROOT/name).read_text() for name in FILES}
class Coverage(unittest.TestCase):
    def refused(self,name,text):
        with tempfile.TemporaryDirectory(prefix='admission-schema-') as temp:
            root=Path(temp)
            for path,source in DATA.items():
                target=root/path;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text if path==name else source)
            with self.assertRaises((ValueError,OSError)):
                checker.check(root)
    def test_current_schema(self):self.assertEqual(checker.check(ROOT),74)
    def test_every_canonical_field_growth_requires_a_decision(self):
        header=DATA[FILES[0]]
        for name in json.loads(DATA['scripts/market_admission_schema.json']):
            with self.subTest(owner=name):
                token=('class ' if name in ['Draft','Journal'] else 'struct ')+name+' {'
                self.refused(FILES[0],header.replace(token,token+'\n    int hidden_storage;'))
    def test_every_per_order_leaf_is_mirrored(self):
        path='scripts/market_admission_mirror_fields.json';rows=json.loads(DATA[path])
        for i,row in enumerate(rows):
            with self.subTest(field=row[0]):
                changed=rows.copy();del changed[i];self.refused(path,json.dumps(changed))
                changed=rows.copy();changed[i]=[row[0],row[1],row[2]+'wrong'];self.refused(path,json.dumps(changed))
                changed=rows.copy();changed[i]=[row[0],'double' if row[1]!='double' else 'int64_t',row[2]];self.refused(path,json.dumps(changed))
    def test_typed_leaf_emissions_cannot_be_removed(self):
        source=DATA['src/market_admission.cpp'];schema=json.loads(DATA['scripts/market_admission_schema.json'])
        # Independent stored-field registry versus hand-written production visitor.
        for owner in ['Configuration','SizingObservation','CommandObservation']:
            for name,kind in schema[owner].items():
                token='F('+name+');'
                if token not in source:continue
                with self.subTest(owner=owner,field=name):self.refused('src/market_admission.cpp',source.replace(token,''))
        for token in ['field(p,"original_sizing_present",o.original_sizing.has_value());',
                      'field(p,"observation_present",bool(o.observation()));',
                      'field(p,"review_present",o.review().has_value());',
                      'field(p,"sizing_revision_present",o.sizing_revision().has_value());',
                      'field(p,"kind",uint64_t(value.index()));',
                      'field(p,"size",uint64_t(values.size()));',
                      'r.field(path,"next_sequence",next_sequence_);',
                      'r.field(path,"active_allocations",active_allocations_);',
                      'field(p,"target_command",o.target_command);',
                      'field(p,"peer_incarnation",o.peer_incarnation);',
                      'field(p,"transaction_quantity",o.transaction_quantity);',
                      'field(p,"cause_fill",o.cause_fill);',
                      'field(p,"cursor_domain",o.cursor().domain());']:
            with self.subTest(token=token):self.refused('src/market_admission.cpp',source.replace(token,''))
    def test_each_retained_array_and_optional_payload_is_owned(self):
        source=DATA['src/market_admission.cpp']
        for token in ['array(o.before,','array(o.removed,','array(o.book,','array(o.reviewed,','array(o.resolutions,','array(o.causes,','r.array(events_,','r.array(outstanding_sequences_,',
                      'if(o.observation())command(*o.observation(),p+".observation");',
                      'if(o.review())review(*o.review(),p+".review");',
                      'if(o.sizing_revision())revision(*o.sizing_revision(),p+".sizing_revision");',
                      'if(o.original_sizing)sizing(*o.original_sizing,p+".original_sizing");']:
            with self.subTest(token=token):self.refused('src/market_admission.cpp',source.replace(token,token.replace('o.','foreign.').replace('events_','foreign_').replace('outstanding_sequences_','foreign_')))
    def test_discriminators_and_waivers(self):
        header=DATA[FILES[0]]
        for old,new in [('Entry, Raw, Cancel, CancelAll','Entry, Raw, CancelAll, Cancel'),
                        ('DefaultGross, ExplicitPair, TerminalGross','DefaultGross, ExplicitPair, TerminalGross, Hidden'),
                        ('Original, Rejected, PairedTransaction','Original, PairedTransaction, Rejected'),
                        ('std::variant<CommandEvent, ReviewEvent, SizingEvent>','std::variant<CommandEvent, SizingEvent, ReviewEvent>')]:
            with self.subTest(old=old):self.refused(FILES[0],header.replace(old,new))
        for path in ['scripts/broker_state_hash_waivers.txt','scripts/pending_order_mirror_waivers.txt']:
            self.refused(path,DATA[path]+'\nmarket_admission # forbidden\n')
    def test_hash_consumes_the_actual_reflection(self):
        path='src/engine_state_hash.cpp';source=DATA[path]
        for token in ['admission::reflect(o.market_admission,','market_admission_journal_.reflect(','f.s(field.path);f.u(field.value.index());']:
            with self.subTest(token=token):self.refused(path,source.replace(token,'/*removed*/'))
if __name__=='__main__':unittest.main()

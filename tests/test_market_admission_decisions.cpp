// Independent native literals. No Pine, external tape, reference rows or grader.
#include "admission_literal_book.hpp"
#include <cstdio>
#include <functional>
using namespace admission_test;
int checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);}}while(0)
void pair_settlement(){Book b;b.add("S",3,false);b.add("B",2);b.pair();
    CHECK(b.live("S")&&b.live("B"));CHECK(b.get("S").paired_flat_market_transaction_qty==3);CHECK(b.get("B").paired_flat_market_transaction_qty==5);
    b.next_bar();b.fire("B",false);CHECK(b.position()==5&&b.has("B")&&b.live("S"));
    b.fire("S",false);CHECK(b.position()==2&&b.trades()==1);CHECK(b.retired.size()==2);b.compact();CHECK(b.size()==0);
}
void rejected_third(){Book b;b.add("S",3,false);b.add("B",2);b.add("huge",100000);CHECK(!b.has("huge"));b.pair();CHECK(b.live("S")&&b.live("B"));b.next_bar();b.fire("B",false);b.fire("S");CHECK(b.position()==2);
    Book t;t.equity(450);t.terminal_mode();t.add("S",3,false);t.add("B",2);t.add("huge",100000);CHECK(!t.has("huge"));t.terminal();CHECK(t.has("B"));
    t.fire("S",false);t.fire("B");CHECK(t.position()==2);
    Book clean;clean.equity(450);clean.terminal_mode();clean.add("S",3,false);clean.add("B",2);clean.terminal();CHECK(!clean.has("B"));clean.fire("S");CHECK(clean.position()==-3);
}
void no_target_cancel(){Book p;p.add("S",3,false);p.add("B",2);p.cancel("absent");p.pair();CHECK(p.live("S")&&p.live("B"));
    Book d;d.default_mode();d.add("L",missing);d.add("S",missing,false);d.cancel("absent");d.defaults();CHECK(d.size()==2);CHECK(!d.mirror("L").default_flat_market_gross_candidate);d.next_bar();d.fire("L",false);d.fire("S");CHECK(d.position()==-10);
    Book plain;plain.default_mode();plain.add("L",missing);plain.add("S",missing,false);plain.defaults();CHECK(plain.has("L")&&!plain.has("S"));plain.next_bar();plain.fire("L");CHECK(plain.position()==10);
}
void review_and_config(){Book one;one.add("A",3);one.pair();CHECK(!one.mirror("A").paired_flat_market_candidate);CHECK(one.mirror("A").paired_flat_market_own_qty==3);one.add("B",2,false);one.pair();CHECK(!one.live("A")&&!one.live("B"));
    Book changed;changed.add("A",3);changed.add("B",2,false);changed.risk_limit(100);changed.pair();changed.risk_limit(0);changed.pair();CHECK(!changed.live("A")&&!changed.live("B"));
    Book live;live.add("A",3);live.add("B",2,false);live.pair();live.risk_limit(100);CHECK(!live.live("A"));live.risk_limit(0);CHECK(live.live("A"));
}
void replacement(){Book m;m.add("A",3);m.add("A",100000);CHECK(m.has("A")&&m.get("A").incarnation==41);m.add("A",100000,true,120);CHECK(!m.has("A"));
    Book r;r.add("A",3);r.add("B",2,false);const auto seq=r.get("A").created_seq;r.add("A",3);CHECK(r.get("A").created_seq==seq&&r.get("A").incarnation==43);r.pair();CHECK(!r.live("A")&&!r.live("B"));
}
void original_and_fee(){Book all;all.equity(150);all.default_mode(100);all.add("A",missing);Book part;part.equity(150);part.default_mode(80);part.add("A",missing);
    CHECK(all.get("A").frozen_default_qty==1&&part.get("A").frozen_default_qty==1);CHECK(all.mirror("A").opening_affordability_exemption_candidate==1);CHECK(part.mirror("A").opening_affordability_exemption_candidate==0);part.pct(100);CHECK(part.mirror("A").opening_affordability_exemption_candidate==0);
    all.next_bar();all.fire("A");CHECK(all.position()==1&&all.opening());if(all.opening())CHECK(all.opening()->decision()==broker::OpeningDecision::Exempt);
    Book fee;fee.equity(150);fee.default_mode();fee.fee(0.1);fee.add("A",missing);fee.next_bar();fee.fire("A");CHECK(fee.position()==1&&fee.opening());if(fee.opening())CHECK(fee.opening()->decision()==broker::OpeningDecision::Check);CHECK(fee.lots().size()==1);if(!fee.lots().empty())CHECK(std::abs(fee.lots()[0].entry_commission_account-0.1)<1e-12);
}
void margin_revision(){Book b;b.margin(50);b.add("explicit",1);b.raw("seed",12,false);b.fire("seed");b.margin(100);b.default_mode();b.add("default",missing);CHECK(b.get("default").frozen_default_qty==10);const auto before=b.mirror("default");const auto explicit_before=b.mirror("explicit");
    b.liquidate_and_refresh(105);CHECK(b.trades()>0&&std::abs(b.position())<12);CHECK(b.get("default").frozen_default_qty==9);CHECK(b.get("default").sizing_equity==940);CHECK(b.mirror("default").opening_affordability_exemption_candidate==before.opening_affordability_exemption_candidate);CHECK(b.mirror("explicit").explicit_placement_equity==explicit_before.explicit_placement_equity);CHECK(b.get("explicit").affordability_placement_equity==940);
    std::printf("margin revision: old qty10/E1000 -> qty%.17g/E%.17g; actual position%.17g trades%zu\n",b.get("default").frozen_default_qty,b.get("default").sizing_equity,b.position(),b.trades());
}
int main(){const std::pair<const char*,void(*)()> tests[]={{"paired committed peer/settlement",pair_settlement},{"rejected-third asymmetry",rejected_third},{"no-target cancel asymmetry",no_target_cancel},{"review/config history",review_and_config},{"replacement order",replacement},{"original qualification/actual fee",original_and_fee},{"actual margin sizing revision",margin_revision}};
    for(auto t:tests){std::printf("case: %s\n",t.first);try{t.second();}catch(const std::exception&e){++failures;std::fprintf(stderr,"FAIL %s: %s\n",t.first,e.what());}}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}

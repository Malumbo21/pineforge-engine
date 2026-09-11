// Additional independent literals and all-field native ABI before/after evidence.
// No tape, Pine, references, grading or generated strategy execution.
#include "admission_literal_book.hpp"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
namespace prior {
#include "fixtures/market_admission/cc0_pending_order_mirror.hpp"
}
using namespace admission_test;
namespace {
int checks=0,failures=0,mirrors=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);}}while(0)
class Matrix:public Book{
public:
    void observe(const char* stage,const char* id){
        auto m=mirror(id);CHECK(m.size==sizeof(m));++mirrors;
        std::printf("MIRROR %s %s\n",stage,id);
        // size reports this producer's sizeof and necessarily grows on append;
        // every other legacy field is compared byte-for-byte across the ABIs.
#define OLD_FIELD(field,ctype) if(std::strcmp(#field,"size")!=0){std::printf("%s=",#field);const auto* p=reinterpret_cast<const unsigned char*>(&m.field);for(size_t j=0;j<sizeof(m.field);++j)std::printf("%02x",p[j]);std::puts("");}
#include "fixtures/market_admission/cc0_fields.inc"
#undef OLD_FIELD
    }
    void bracket(const char* id,const char* binding=""){strategy_exit(id,binding,130,90);}
    void group(const char* id,double qty){strategy_entry(id,true,missing,missing,qty,"","G",2);}
    void close_seed(){strategy_close("seed","",1,missing,true);}
    void fx(double q){set_syminfo_metadata("account_currency_fx",q);}
    void pv(double q){set_syminfo_pointvalue(q);}
};
void source_order_and_budgets(){
    for(bool buy_first:{false,true})for(double equity:{450.0,500.0,1000.0}){
        Matrix b;b.equity(equity);b.add("A",3,buy_first);b.add("B",2,!buy_first);
        b.observe("budget-before","A");b.observe("budget-before","B");b.pair();
        CHECK(b.has("B")== (equity>=500));
        b.observe("budget-reviewed","A");if(b.has("B"))b.observe("budget-reviewed","B");
        b.next_bar();
        if(equity<500){b.fire("A");CHECK(b.position()==(buy_first?3:-3));}
        else {b.fire(buy_first?"A":"B",false);b.fire(buy_first?"B":"A");CHECK(b.position()==(buy_first?-2:2));CHECK(b.trades()==1);}
    }
    Matrix q;q.add("A",5.1);q.add("B",5.1,false);q.pair();CHECK(q.get("A").paired_flat_market_transaction_qty==5&&q.get("B").paired_flat_market_transaction_qty==10);
    q.observe("quantized-own","A");q.observe("quantized-own","B");q.next_bar();q.fire("A",false);q.fire("B");CHECK(q.position()==-5);
    Matrix gap;gap.add("S",3,false);gap.add("B",2);gap.next_bar(210);gap.pair();CHECK(!gap.has("B")&&gap.has("S"));gap.observe("buy-gap-rejection","S");
}
void default_live_cap_and_current_fx(){
    for(bool long_side:{false,true}){
        Matrix b;b.raw("seed",5,long_side);b.fire("seed");b.next_bar();b.default_mode();b.add("A",missing);b.add("B",missing,false);
        b.observe("live-default-before","A");b.observe("live-default-before","B");b.defaults();CHECK(b.has("B")==long_side);
        b.observe("live-default-reviewed","A");if(b.has("B"))b.observe("live-default-reviewed","B");
        b.next_bar();b.fire("A",false);if(long_side){CHECK(b.position()==5);b.fire("B");CHECK(b.position()==-10);}else{b.compact();CHECK(b.position()==10);}
    }
    Matrix fx;fx.default_mode();fx.add("A",missing);fx.add("B",missing,false);fx.fx(0.5);fx.defaults();CHECK(fx.size()==2);
    fx.observe("current-fx-default","A");fx.observe("current-fx-default","B");fx.next_bar();fx.fire("A",false);fx.fire("B");CHECK(fx.position()==-10);
    Matrix pair;pair.add("S",3,false);pair.add("B",2);pair.fx(2);pair.pv(2);pair.next_bar(110);pair.pair();CHECK(pair.live("B"));pair.observe("captured-review-current-fill","B");pair.fire("B");CHECK(pair.position()==0&&!pair.has("B"));
    Matrix terminal;terminal.equity(450);terminal.terminal_mode();terminal.add("S",3,false);terminal.add("B",2);terminal.fx(0.5);terminal.terminal();CHECK(terminal.size()==2);
    terminal.observe("current-fx-terminal","S");terminal.observe("current-fx-terminal","B");terminal.fire("S",false);terminal.fire("B");CHECK(terminal.position()==2);
}
void whole_book_and_command_receipts(){
    Matrix ordinary;ordinary.add("S",3,false);ordinary.bracket("X","S");ordinary.add("B",2);ordinary.pair();CHECK(ordinary.live("S")&&ordinary.live("B"));
    ordinary.observe("interleaved-bracket","S");ordinary.observe("interleaved-bracket","X");ordinary.observe("interleaved-bracket","B");
    Matrix terminal;terminal.equity(450);terminal.terminal_mode();terminal.add("S",3,false);terminal.bracket("X","S");terminal.add("B",2);terminal.terminal();CHECK(terminal.has("S")&&terminal.has("B"));
    terminal.observe("terminal-bracket-fallback","S");terminal.observe("terminal-bracket-fallback","B");
    Matrix named;named.add("P",1,true,120);named.bracket("X","P");const auto canceled=named.get("P").incarnation;named.cancel("P");named.add("P",100000);CHECK(!named.has("P"));named.add("P",1);CHECK(named.get("P").recreated_after_named_cancelled_entry_incarnation==0&&named.get("P").incarnation!=canceled);
    named.observe("consumed-named-cancel","P");named.observe("consumed-named-cancel","X");
    Matrix oca;oca.group("A",2);oca.observe("oca-original","A");oca.reduce("G",1);CHECK(oca.get("A").qty==1);oca.observe("oca-reduced","A");oca.next_bar();oca.fire("A");CHECK(oca.position()==1);
    Matrix closed;closed.raw("seed",1);closed.fire("seed");closed.close_seed();CHECK(closed.position()==0);closed.add("A",3);closed.add("B",2,false);
    CHECK(closed.mirror("A").paired_flat_market_candidate==1&&closed.mirror("A").explicit_flat_admission_candidate==0);
    closed.pair();CHECK(closed.live("A")&&closed.live("B"));closed.observe("real-close-before-entry","A");closed.observe("real-close-before-entry","B");
    Matrix invalid;invalid.add("infinite",std::numeric_limits<double>::infinity());CHECK(invalid.has("infinite")&&invalid.mirror("infinite").explicit_flat_admission_candidate==1);invalid.observe("legacy-nonfinite-placement","infinite");
}
}
int main(){
    const std::pair<const char*,void(*)()> groups[]={{"source order/budgets",source_order_and_budgets},{"live cap/current FX",default_live_cap_and_current_fx},{"whole book/receipts",whole_book_and_command_receipts}};
    for(auto t:groups){std::printf("CASE %s\n",t.first);try{t.second();}catch(const std::exception& e){++failures;std::fprintf(stderr,"FAIL %s: %s\n",t.first,e.what());}}
    std::printf("MATRIX %d checks, %d mirrors, %d failures\n",checks,mirrors,failures);return failures?1:0;
}

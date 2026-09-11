#include <pineforge/exit_leg_lifecycle.hpp>
#include <cstdio>
using namespace pineforge::exit_legs;
int main() {
    Lifecycle predecessor; predecessor.attach(10,1); predecessor.set_stop_price(95);
    Lifecycle order; order.attach(11,1); order.set_stop_price(90);
    Frame requested{1,10,Domain::Ordinary,Phase::Observation};
    Action stage{order.target(),order.revision(),requested,
        StageReplacement{{10,predecessor.definition(10),Barrier{requested}}}};
    if(order.apply(order.target(),stage)!=Result::Applied || !order.pending_replacement()) return 2;
    Frame earlier_completion{2,9,Domain::Ordinary,Phase::AfterMargin};
    Action complete{order.target(),order.revision(),earlier_completion,CompleteBarrier{earlier_completion,order.release_barrier()}};
    auto result=order.apply(order.target(),complete);
    std::printf("barrier created at bar10; completion at bar9: result=%d pending=%d\n",int(result),order.pending_replacement());
    if(result==Result::Applied && !order.pending_replacement()) {
        std::puts("ROOT CONTRACT FAILURE: an earlier unrelated completion releases the outstanding replacement.");
        return 1;
    }
    return 0;
}

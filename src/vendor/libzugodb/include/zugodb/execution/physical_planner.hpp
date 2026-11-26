#pragma once

#include "zugodb/execution/physical_operator.hpp"
#include "zugodb/planner/logical_operator.hpp"
#include "zugodb/catalog/catalog.hpp"

namespace zugodb {

class PhysicalPlanner {
public:
    explicit PhysicalPlanner(Catalog &catalog) : catalog_(catalog) {}

    PhysicalOpPtr Plan(const LogicalOperator &logical);

private:
    PhysicalOpPtr PlanGet(const LogicalGet &op);
    PhysicalOpPtr PlanFilter(const LogicalFilter &op);
    PhysicalOpPtr PlanProjection(const LogicalProjection &op);
    PhysicalOpPtr PlanOrderBy(const LogicalOrderBy &op);
    PhysicalOpPtr PlanLimit(const LogicalLimit &op);
    PhysicalOpPtr PlanInsert(const LogicalInsert &op);
    PhysicalOpPtr PlanCreateTable(const LogicalCreateTable &op);
    PhysicalOpPtr PlanDropTable(const LogicalDropTable &op);
    PhysicalOpPtr PlanWindow(const LogicalWindow &op);
    PhysicalOpPtr PlanDistinct(const LogicalDistinct &op);
    PhysicalOpPtr PlanAggregate(const LogicalAggregate &op);
    PhysicalOpPtr PlanJoin(const LogicalJoin &op);
    PhysicalOpPtr PlanUpdateOp(const LogicalUpdate &op);
    PhysicalOpPtr PlanDeleteOp(const LogicalDeleteOp &op);
    PhysicalOpPtr PlanDummyScan(const LogicalDummyScan &op);

    Catalog &catalog_;
};

} // namespace zugodb

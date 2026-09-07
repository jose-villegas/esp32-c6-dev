#include "sand_work_counters.h"

#if CONFIG_LAUNCHER_DEVELOPMENT

sand_work_counters_t sand_work_counters;

void sand_work_counters_reset(void)
{
    sand_work_counters = (sand_work_counters_t){ 0 };
}

void sand_work_counters_dump(void (*emit)(const char *name, uint32_t value))
{
    emit("xflow_calls", sand_work_counters.xflow_calls);
    emit("rows_walked", sand_work_counters.rows_walked);
    emit("blocks_considered", sand_work_counters.blocks_considered);
    emit("blocks_examined", sand_work_counters.blocks_examined);
    emit("cells_examined", sand_work_counters.cells_examined);
    emit("cells_passing_mask", sand_work_counters.cells_passing_mask);
    emit("equalise_one_cell_calls", sand_work_counters.equalise_one_cell_calls);
    emit("find_shallowest_calls", sand_work_counters.find_shallowest_calls);
    emit("find_shallowest_iters", sand_work_counters.find_shallowest_iters);
    emit("transfers", sand_work_counters.transfers);
    emit("move_liquid_grain_calls", sand_work_counters.move_liquid_grain_calls);
}

#endif /* CONFIG_LAUNCHER_DEVELOPMENT */

#pragma once

namespace fpshud::tools {

void print_probe_row(const char* name, bool available, const char* value_or_reason, int& avail_cnt,
                     int& total_cnt);

void run_sensor_probe();

}  // namespace fpshud::tools

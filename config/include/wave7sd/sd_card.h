#pragma once

#include <string>
#include <cstdint>

namespace wave7sd {

bool mount();
void unmount();
bool get_capacity_kb(uint64_t *total_kb, uint64_t *available_kb);
bool is_mounted();
std::string list_dir_json(const std::string &path);
bool read_file_to_string(const std::string &path, std::string &out);
bool delete_file(const std::string &path);
bool append_file_chunk(const std::string &path, const uint8_t *data, size_t len, bool create_if_missing = true);

// CH422G expander helper (opaque pointer to whatever component type is used)
void set_cs_expander(void *expander);

} // namespace wave7sd

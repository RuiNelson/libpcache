#include "cmd_utils.hxx"

bool has_flag(const command &cmd, const std::string &flag) {
    for (const auto &f : cmd.flags) {
        if (f == flag)
            return true;
    }
    return false;
}
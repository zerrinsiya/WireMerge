#pragma once
#include <vector>
#include <string>

namespace wm {

struct DependencyStatus {
    std::string name;
    bool found;
    std::string details;
};

std::vector<DependencyStatus> CheckDependencies();

bool ReportDependencyStatus(const std::vector<DependencyStatus>& statuses);

}

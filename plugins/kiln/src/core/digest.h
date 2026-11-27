#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/hash.h"
#include "core/result.h"
#include "core/task.h"

namespace kiln {

struct OutputRecord {
    std::string path;
    Hash256 hash;
};

struct InputRecord {
    std::string path;
    Hash256 hash;
};

// output_summary_hash(t) = CanonicalHasher over sorted (path, hash) pairs.
Hash256 output_summary_hash(const std::vector<OutputRecord>& outputs);

// INPUT summary: CanonicalHasher over sorted (path, hash) pairs of a task's
// recorded inputs. Used as the dep contribution for outputs_opaque rules
// (their outputs are not byte-reproducible, see decisions.md).
Hash256 input_summary_hash(const std::vector<InputRecord>& inputs);

// Action digest per spec §2.2. Dep output summaries come from the PREVIOUS
// build's traces (or this build's new traces for deps that already ran).
// hash_of(path) must return an error for missing files.
Result<Hash256> compute_action_digest(
    const TaskDecl& decl,
    const std::vector<std::string>& dynamic_deps,
    const std::function<Result<Hash256>(const std::string&)>& hash_of,
    const std::vector<std::pair<TaskId, Hash256>>& dep_output_summaries);

} // namespace kiln

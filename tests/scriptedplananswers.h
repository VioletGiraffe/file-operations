#pragma once

#include "fileoperations/fileoperationtypes.h"

#include <optional>
#include <stddef.h>
#include <vector>

// Answers the plan's unanswered rows from the script, in plan order. A RemainingMatchingIssues answer also fills every
// later row of the same kind, as the list's bulk selection would. nullopt when the script runs out.
inline std::optional<ConflictResolutions> answerPlanFromScript(const ConflictPlan& plan, const std::vector<Decision>& decisions, size_t& nextDecision)
{
	ConflictResolutions resolutions(plan.size());
	for (size_t i = 0; i < plan.size(); ++i)
	{
		if (resolutions[i])
			continue;
		if (plan[i].resolution && !plan[i].renameTargetTakenByAnotherItem)
		{
			resolutions[i] = plan[i].resolution; // Answered in an earlier round
			continue;
		}
		if (nextDecision >= decisions.size())
			return {};

		Decision answer = decisions[nextDecision++];
		if (answer.scope == DecisionScope::RemainingMatchingIssues)
		{
			answer.scope = DecisionScope::ThisItem;
			for (size_t j = i + 1; j < plan.size(); ++j)
				if (plan[j].issue.kind == plan[i].issue.kind && !plan[j].resolution)
					resolutions[j] = answer;
		}
		resolutions[i] = answer;
	}
	return resolutions;
}

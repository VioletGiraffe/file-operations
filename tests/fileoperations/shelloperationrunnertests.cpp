#include "shelloperations/cshelloperationrunner.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include "qtcore_helpers/catch_qt.hpp" // qtutils

#include <QCoreApplication>
RESTORE_COMPILER_WARNINGS

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace {

struct TestApplication
{
	char name[5] = "test";
	char* argv[2] = { name, nullptr };
	int argc = 1;
	QCoreApplication app{ argc, argv };
};

} // namespace

TEST_CASE("The wait dispatches events until every operation has finished", "[shelloperationrunner]")
{
	TestApplication application;
	CShellOperationRunner runner;

	std::atomic<int> finishedOperations = 0;
	int callbacksDelivered = 0;
	for (int i = 0; i < 3; ++i)
	{
		runner.run([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });
			// Blocks until the runner's thread dispatches it: a wait that only joined would deadlock here
			QMetaObject::invokeMethod(&application.app, [&] { ++callbacksDelivered; }, Qt::BlockingQueuedConnection);
			++finishedOperations;
		});
	}

	runner.waitForPendingOperations();
	CHECK(finishedOperations == 3);
	CHECK(callbacksDelivered == 3);
}

TEST_CASE("The wait returns after an operation throws", "[shelloperationrunner]")
{
	TestApplication application;
	CShellOperationRunner runner;

	runner.run([] { throw std::runtime_error{ "operation failed" }; });
	runner.waitForPendingOperations();
	SUCCEED("The throwing operation's entry was removed");
}

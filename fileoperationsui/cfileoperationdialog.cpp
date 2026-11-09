#include "cfileoperationdialog.h"

#include "cconflictlistdialog.h"
#include "cfileoperationprompt.h"


// Submodule includes
#include "assert/advanced_assert.h"
#include "lang/utils.hpp" // mv()
#include "qtcore_helpers/qstring_helpers.hpp" // QSL


DISABLE_COMPILER_WARNINGS
#include "ui_cfileoperationdialog.h"

#include <QCloseEvent>
#include <QDir>
#include <QMessageBox>
#include <QTimer>
RESTORE_COMPILER_WARNINGS

#ifdef _WIN32
#include <Windows.h>
#endif

namespace
{

// How long an operation must run, in milliseconds, before its dialog appears. Short operations finish and
// dispose of themselves within it, so they never flash a window on screen.
constexpr int firstShowDelay = 500;

} // namespace

CFileOperationDialog::CFileOperationDialog(FileOperationRequest request, std::function<QPoint(QSize)> backgroundAnchorProvider,
	QWidget* parent, const uint64_t transferChunkSize) :
	QWidget(parent, Qt::Window),
	ui(new Ui::CFileOperationDialog),
	_operation(promptOperationFor(request)),
	_primaryUnit(primaryProgressUnitFor(request)),
	_routeText(fileOperationRouteText(request)),
	_backgroundAnchorProvider(mv(backgroundAnchorProvider)),
	_job(mv(request), transferChunkSize)
{
	ui->setupUi(this);

	// Blocks the main window while the operation is in the foreground; switchToBackground() lifts it.
	// Must stay WindowModal: other operations' dialogs are siblings, and ApplicationModal would freeze them too.
	setWindowModality(Qt::WindowModal);

#ifdef _WIN32
	// An owned top-level window gets no taskbar button of its own, and the progress indicator below only ever
	// activates for a window that has a button, so we're requesting it. Must precede the first show - that is when the button is created.
	const HWND thisWindow = reinterpret_cast<HWND>(winId());
	::SetWindowLongPtrW(thisWindow, GWL_EXSTYLE, ::GetWindowLongPtrW(thisWindow, GWL_EXSTYLE) | WS_EX_APPWINDOW);
#endif

	ui->_overallProgress->linkToWidgetstaskbarButton(this);

	ui->_lblCurrentFile->clear();
	ui->_lblStatus->clear();
	ui->_lblSummary->hide();
	updateWindowTitle();

	// The per-file byte bar is meaningful only for byte-unit transfers; delete tracks whole items.
	if (_primaryUnit != PrimaryProgressUnit::Bytes)
	{
		ui->_fileProgress->hide();
		ui->_fileProgressText->hide();

		// The item name takes over the hidden bar's columns
		int row = 0, column = 0, rowSpan = 0, columnSpan = 0;
		ui->progressGrid->getItemPosition(ui->progressGrid->indexOf(ui->_lblCurrentFile), &row, &column, &rowSpan, &columnSpan);
		ui->progressGrid->removeWidget(ui->_lblCurrentFile);
		ui->progressGrid->addWidget(ui->_lblCurrentFile, row, column, rowSpan, -1);
	}

	// While running this button cancels (and the dialog lives on until the job reports); once finished it
	// is relabeled Close and simply dismisses the dialog.
	connect(ui->_btnCancel, &QPushButton::clicked, this, [this] {
		if (_result)
			close();
		else
			(void)confirmCancellation();
	});
	connect(ui->_btnPause, &QPushButton::clicked, this, &CFileOperationDialog::togglePause);
	connect(ui->_btnBackground, &QPushButton::clicked, this, &CFileOperationDialog::switchToBackground);

	_eventTimer = new QTimer{ this };
	_eventTimer->setObjectName(QSL("eventTimer")); // Named so a test can find and stop it unambiguously
	_eventTimer->setInterval(100);
	connect(_eventTimer, &QTimer::timeout, this, &CFileOperationDialog::drainEvents);

	adjustSize();
}

void CFileOperationDialog::start()
{
	assert_debug_only(_job.status() == JobStatus::NotStarted);
	_job.start();
	_eventTimer->start();

	// Anything that needs the user before this fires - a decision prompt, an outcome worth reporting - shows
	// the dialog itself; a finished one either disposed of itself or belongs to a caller that decides its
	// own visibility.
	QTimer::singleShot(firstShowDelay, this, [this] {
		if (!_result)
			show();
	});
}

CFileOperationDialog::~CFileOperationDialog()
{
	// The job destructor requests cancellation and joins; nothing here may outlive that.
	delete ui;
}

void CFileOperationDialog::drainEvents()
{
	if (_draining)
		return; // A modal prompt is spinning a nested event loop; do not re-enter the drain

	_draining = true;
	_job.processEvents(*this);
	_draining = false;
}

void CFileOperationDialog::renderProgress(const ProgressSnapshot& snapshot)
{
	if (snapshot.currentEntry)
		ui->_lblCurrentFile->setText(QDir::toNativeSeparators(snapshot.currentEntry->value()));
	ui->_lblStatus->setText(fileOperationProgressStatusText(snapshot, _operation, _primaryUnit));

	if (snapshot.phase == OperationPhase::Scanning)
	{
		// Indeterminate overall bar: no total, no ETA yet.
		ui->_overallProgress->setMaximum(0);
		ui->_overallProgressText->clear();
		return;
	}

	_percentDone = overallPercent(snapshot, _primaryUnit);
	if (_percentDone)
	{
		ui->_overallProgress->setMaximum(100);
		ui->_overallProgress->setValue(*_percentDone);
		ui->_overallProgressText->setText(QString::number(*_percentDone) % QLatin1Char('%'));
	}
	else
	{
		ui->_overallProgress->setMaximum(0); // Totals not yet known (a manifest is still pending)
		ui->_overallProgressText->clear();
	}
	updateWindowTitle();

	if (_primaryUnit != PrimaryProgressUnit::Bytes)
		return;
	if (const std::optional<int> filePercent = currentEntryPercent(snapshot))
	{
		ui->_fileProgress->setValue(*filePercent);
		ui->_fileProgressText->setText(QString::number(*filePercent) % QLatin1Char('%'));
	}
}

Decision CFileOperationDialog::askDecision(const DecisionRequest& request)
{
	show(); // The modal prompt must not stand alone on screen if the first show is still pending
	ui->_overallProgress->setState(psStopped);
	const Decision decision = presentDecision(request);
	ui->_overallProgress->setState(_paused ? psPaused : psNormal);
	return decision;
}

std::optional<ConflictResolutions> CFileOperationDialog::askConflictPlan(const ConflictPlan& plan)
{
	show();
	ui->_overallProgress->setState(psStopped);
	auto resolutions = presentConflictPlan(plan);
	if (!resolutions)
		disableControlsAfterCancellation(); // The job cancels on this answer
	ui->_overallProgress->setState(_paused ? psPaused : psNormal);
	return resolutions;
}

void CFileOperationDialog::onFinished(const OperationSummary& summary)
{
	_result = summary;
	_eventTimer->stop();

	ui->_overallProgress->setState(summary.status == CompletionStatus::Failed ? psStopped : psNormal);
	ui->_fileProgress->hide();
	ui->_fileProgressText->hide();
	ui->_lblCurrentFile->hide();
	ui->_lblStatus->hide();
	updateWindowTitle();
	ui->_btnPause->hide();
	ui->_btnBackground->hide();
	ui->_btnCancel->setText(tr("Close"));
	ui->_btnCancel->setEnabled(true);

	ui->_lblSummary->setText(fileOperationSummaryText(summary, _operation));
	ui->_lblSummary->show();

	if (_completionHandler)
		_completionHandler(summary);

	// An embedded or component-test dialog is owned and inspected by its caller; only a self-disposing
	// production dialog manages its own lifetime past completion.
	if (!_selfDisposes)
		return;

	// Nothing left for the user to act on, or a dialog they already dismissed: leave no window behind.
	if (_dismissedWhileRunning || !fileOperationNeedsAttention(summary))
	{
		deleteLater();
		return;
	}

	if (_isInBackgroundMode)
	{
		_isInBackgroundMode = false; // Back to the foreground: the outcome needs the user's attention
		adjustSize(); // Refit: backgrounding shrank the window to the detail widgets it has now hidden
		raise();
		activateWindow();
	}
	else if (!isVisible())
		show(); // Finished before the delayed first show, with an outcome worth showing
}

Decision CFileOperationDialog::presentDecision(const DecisionRequest& request)
{
	CFileOperationPrompt prompt{ request, _operation, this };
	return prompt.ask();
}

std::optional<ConflictResolutions> CFileOperationDialog::presentConflictPlan(const ConflictPlan& plan)
{
	CConflictListDialog list{ plan, _operation, this };
	return list.ask();
}

void CFileOperationDialog::requestCancellation()
{
	_job.cancel();
	disableControlsAfterCancellation();
}

void CFileOperationDialog::disableControlsAfterCancellation()
{
	ui->_btnCancel->setEnabled(false);
	ui->_btnPause->setEnabled(false);
}

void CFileOperationDialog::setPaused(const bool paused)
{
	if (_paused == paused)
		return;

	_paused = paused;
	_job.setPaused(paused);
	ui->_btnPause->setText(_paused ? tr("Resume") : tr("Pause"));
	ui->_overallProgress->setState(_paused ? psPaused : psNormal);
	updateWindowTitle();
}

void CFileOperationDialog::togglePause()
{
	setPaused(!_paused);
}

void CFileOperationDialog::switchToBackground()
{
	ui->_lblCurrentFile->hide();
	ui->_fileProgress->hide();
	ui->_fileProgressText->hide();
	ui->_btnBackground->hide();

	// Queued: hiding the detail widgets only posts a deferred LayoutRequest, so an adjustSize() in this
	// stack would measure the not-yet-relaid-out size.
	QMetaObject::invokeMethod(this, [this] {
		// Qt admits and releases modal windows as they are shown and hidden, so releasing the main window means
		// recycling this one. The resize and the move ride along while it is hidden, keeping it to one transition.
		hide();
		setWindowModality(Qt::NonModal);

		adjustSize(); // Shrink to fit now that the per-file detail widgets are hidden
		if (_backgroundAnchorProvider)
			move(_backgroundAnchorProvider(frameGeometry().size()));

		show();
		// Hands the keyboard back; as a child window this dialog stays visible above the main window regardless.
		if (QWidget* const owner = parentWidget())
			owner->activateWindow();

		_isInBackgroundMode = true;
	}, Qt::QueuedConnection);
}

bool CFileOperationDialog::confirmCancellation()
{
	if (_result) // Already finished: the button now means Close
		return true;

	const bool wasPaused = _paused;
	if (!wasPaused)
		setPaused(true);

	// This modal is not reached through drainEvents, so guard the drain explicitly: a decision prompt
	// must not stack on top of the confirmation while the worker keeps reporting. Save and restore rather
	// than clear, so a call already nested inside a drain leaves that outer guard intact.
	const bool wasDraining = _draining;
	_draining = true;
	const auto answer = QMessageBox::question(this, tr("Cancel?"), tr("Are you sure you want to cancel this operation?"),
		QMessageBox::Yes | QMessageBox::No);
	_draining = wasDraining;

	if (answer == QMessageBox::Yes)
	{
		requestCancellation();
		return true;
	}

	if (!wasPaused && !_result)
		setPaused(false);
	return false;
}

void CFileOperationDialog::updateWindowTitle()
{
	QString title = fileOperationProgressVerb(_operation);
	if (!_routeText.isEmpty())
		title += QLatin1Char(' ') % _routeText;

	// A finished job is neither in progress nor paused: cancelling while paused leaves _paused set
	if (!_result)
	{
		if (_paused)
			title.prepend(tr("(paused)") % QLatin1Char(' '));
		if (_percentDone)
			title.prepend(QString::number(*_percentDone) % QSL("% "));
	}
	setWindowTitle(title);
}

void CFileOperationDialog::closeEvent(QCloseEvent* e)
{
	if (_result)
	{
		QWidget::closeEvent(e);
		if (_selfDisposes)
			deleteLater(); // The user dismissed a finished dialog; a self-disposing one lives no longer
		return;
	}

	// A running operation asks first and, on confirmation, stays alive (hidden) until it truly ends;
	// onFinished then disposes of it.
	if (confirmCancellation())
	{
		_dismissedWhileRunning = true;
		QWidget::closeEvent(e);
	}
	else
		e->ignore();
}

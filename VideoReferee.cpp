#include "VideoReferee.h"

#include <QAction>
#include <QApplication>
#include <QCameraDevice>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMediaDevices>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QVBoxLayout>
#include <cstdio>
#include <cstring>

// ============================================================
//  Speed steps
// ============================================================
const double VideoRefereeWindow::kSpeedSteps[] = {0.125, 0.25, 0.5, 1.0,
                                                  2.0,   4.0,  8.0};
static constexpr int kNumSpeedSteps = 7;
static constexpr int kDefaultSpeedIdx = 3;

// TCP Interface
static constexpr const char* kOk = "< OK";

// ============================================================
//  DEADBEEF format helpers
// ============================================================
static constexpr uint32_t kFrameMagic = 0xDEADBEEFu;

static inline uint32_t toBE32(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8) |
           ((v & 0x0000FF00u) << 8) | ((v & 0x000000FFu) << 24);
}
static inline uint32_t fromBE32(uint32_t v) {
    return toBE32(v);
}

// ============================================================
//  CaptureWorker
// ============================================================
CaptureWorker::CaptureWorker(const QString& outputPath, int cameraIndex,
                             QObject* parent)
    : QObject(parent), m_outputPath(outputPath), m_cameraIndex(cameraIndex) {
}

CaptureWorker::~CaptureWorker() {
}

void CaptureWorker::requestRotate(const QString& newOutputPath) {
    QMutexLocker lk(&m_rotateMutex);
    m_newOutputPath = newOutputPath;
}

void CaptureWorker::start() {
    m_running = true;

#ifdef _WIN32
    cv::VideoCapture cap(m_cameraIndex, cv::CAP_DSHOW);
#else
    cv::VideoCapture cap(m_cameraIndex);
#endif
    if (!cap.isOpened()) {
        emit errorOccurred(
            QString("Cannot open camera %1.").arg(m_cameraIndex));
        emit finished();
        return;
    }

    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1920);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);
    cap.set(cv::CAP_PROP_FPS, 50);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    double actualFps = cap.get(cv::CAP_PROP_FPS);
    if (actualFps < 1.0 || actualFps > 120.0) {
        actualFps = 50.0;
    }
    emit fpsDetected(actualFps);

    FILE* outFile = fopen(m_outputPath.toLocal8Bit().constData(), "wb");
    if (!outFile) {
        emit errorOccurred(
            QString("Cannot open output file: %1").arg(m_outputPath));
        emit finished();
        return;
    }

    cv::Mat frame, rgb, small;
    std::vector<uchar> jpegBuf;
    std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, 75};

    while (m_running) {
        if (!cap.read(frame) || frame.empty()) {
            QThread::msleep(5);
            continue;
        }
        // ---- Check for pause request (STOPCAPTURE) ----
        if (m_recordingPaused && outFile) {
            fflush(outFile);
            fclose(outFile);
            QString closedPath = m_outputPath;
            outFile = nullptr;
            emit pauseDone(closedPath);
        }

        // ---- While paused: keep grabbing frames for display, skip write ----
        if (m_recordingPaused) {
            // Check for resume
            QString resumePath;
            {
                QMutexLocker lk(&m_rotateMutex);
                resumePath = m_resumePath;
                m_resumePath.clear();
            }
            if (!resumePath.isEmpty()) {
                m_outputPath = resumePath;
                outFile = fopen(m_outputPath.toLocal8Bit().constData(), "wb");
                if (!outFile) {
                    emit errorOccurred(
                        QString("Cannot open new file: %1").arg(m_outputPath));
                    emit finished();
                    return;
                }
                m_recordingPaused = false;
            } else {
                // Still paused — emit preview but skip recording
                cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
                QImage img(small.data, small.cols, small.rows,
                           static_cast<int>(small.step), QImage::Format_RGB888);
                emit frameCaptured(img.copy());
                continue;
            }
        }

        // ---- Check for rotation request ----
        QString newPath;
        {
            QMutexLocker lk(&m_rotateMutex);
            newPath = m_newOutputPath;
            m_newOutputPath.clear();
        }
        if (!newPath.isEmpty()) {
            fflush(outFile);
            fclose(outFile);
            QString closedPath = m_outputPath;
            m_outputPath = newPath;

            outFile = fopen(m_outputPath.toLocal8Bit().constData(), "wb");
            if (!outFile) {
                emit errorOccurred(
                    QString("Cannot open new file: %1").arg(m_outputPath));
                emit finished();
                return;
            }
            emit rotationDone(closedPath);
        }

        // ---- Burn timestamp ----
        std::string ts = QDateTime::currentDateTime()
                             .toString("yyyy-MM-dd  hh:mm:ss.zzz")
                             .toStdString();
        int fontFace = cv::FONT_HERSHEY_SIMPLEX;
        double scale = 0.7;
        int thick = 2, baseline = 0;
        cv::Size tsz = cv::getTextSize(ts, fontFace, scale, thick, &baseline);
        cv::Point org(frame.cols - tsz.width - 10, tsz.height + 10);
        cv::putText(frame, ts, org + cv::Point(1, 1), fontFace, scale,
                    cv::Scalar(0, 0, 0), thick + 1, cv::LINE_AA);
        cv::putText(frame, ts, org, fontFace, scale, cv::Scalar(255, 255, 255),
                    thick, cv::LINE_AA);

        // ---- Record ----
        qint64 filePos = static_cast<qint64>(ftell(outFile));
        cv::imencode(".jpg", frame, jpegBuf, jpegParams);
        uint32_t magic = toBE32(kFrameMagic);
        uint32_t len = toBE32(static_cast<uint32_t>(jpegBuf.size()));
        fwrite(&magic, 4, 1, outFile);
        fwrite(&len, 4, 1, outFile);
        fwrite(jpegBuf.data(), 1, jpegBuf.size(), outFile);

        emit frameIndexed(filePos);

        // ---- Display ----
        cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
        QImage img(rgb.data, rgb.cols, rgb.rows,
                   static_cast<int>(rgb.step), QImage::Format_RGB888);           
        emit frameCaptured(img.copy());
    }

    fflush(outFile);
    fclose(outFile);
    emit finished();
}

void CaptureWorker::stop() {
    m_running = false;
}

void CaptureWorker::pauseRecording() {
    m_recordingPaused = true;  // capture loop will flush+close file
}

void CaptureWorker::resumeRecording(const QString& newOutputPath) {
    QMutexLocker lk(&m_rotateMutex);
    m_resumePath = newOutputPath;
}

// ============================================================
//  VideoRefereeWindow
// ============================================================
int VideoRefereeWindow::selectCamera() {
    const auto devices = QMediaDevices::videoInputs();
    if (devices.isEmpty()) {
        QMessageBox::critical(nullptr, "No cameras found",
                              "No video capture devices were detected.");
        return -1;
    }
    if (devices.size() == 1) {
        return 0;
    }
    return showCameraDialog(nullptr, "Select Camera");
}

QString VideoRefereeWindow::newRecordingPath() const {
    return m_recordingDir + "/recording_" +
           QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".mjpeg";
}

VideoRefereeWindow::VideoRefereeWindow(int cameraIndex, QWidget* parent)
    : QMainWindow(parent), m_cameraIndex(cameraIndex) {
    setWindowTitle("Video Referee");
    resize(1280, 740);

    m_display = new QLabel(this);
    m_display->setAlignment(Qt::AlignCenter);
    m_display->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_display->setStyleSheet("background-color: black;");
    setCentralWidget(m_display);

    m_statusBar = statusBar();

    // ---- Menu ----
    QMenu* camMenu = menuBar()->addMenu("&Camera");
    connect(camMenu->addAction("&Switch Camera…"), &QAction::triggered, this,
            &VideoRefereeWindow::onSwitchCamera);

    QMenu* recMenu = menuBar()->addMenu("&Recording");
    QAction* rotateAct = recMenu->addAction("&Finalize and start new file\tF1");
    rotateAct->setShortcut(Qt::Key_F1);
    connect(rotateAct, &QAction::triggered, this,
            &VideoRefereeWindow::onRotateFiles);

    // ---- Recording dir ----
    m_recordingDir = QDir::homePath() + "/VideoReferee";
    QDir().mkpath(m_recordingDir);

    m_playbackTimer = new QTimer(this);
    connect(m_playbackTimer, &QTimer::timeout, this,
            &VideoRefereeWindow::onPlaybackTimer);

    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, this,
            &VideoRefereeWindow::onStatusTimer);
    m_statusTimer->start(1000);

    // ---- Timer ----
    m_markerTimer = new QTimer(this);
    m_markerTimer->setSingleShot(true);
    connect(m_markerTimer, &QTimer::timeout, [this]() {
        m_showMarker = false;
        // Redraw current frame to clear the overlay
        if (!m_isLive) {
            seekTo(m_replayFrame);
        } else {
            showFrame(m_liveFrame, false);
        }
    });
    m_currentRecordingPath = newRecordingPath();
    startCapture(m_cameraIndex, m_currentRecordingPath);
    initTcpServer();
}

VideoRefereeWindow::~VideoRefereeWindow() {
    if (m_replayFile) {
        fclose(m_replayFile);
        m_replayFile = nullptr;
    }
}

void VideoRefereeWindow::startCapture(int cameraIndex,
                                      const QString& outputPath) {
    m_cameraIndex = cameraIndex;
    m_currentRecordingPath = outputPath;
    m_statusBar->showMessage(QString("Opening camera %1…").arg(cameraIndex));

    m_captureThread = new QThread(this);
    m_captureWorker = new CaptureWorker(outputPath, cameraIndex);
    m_captureWorker->moveToThread(m_captureThread);

    connect(m_captureThread, &QThread::started, m_captureWorker,
            &CaptureWorker::start);
    connect(m_captureWorker, &CaptureWorker::frameCaptured, this,
            &VideoRefereeWindow::onFrameCaptured, Qt::QueuedConnection);
    connect(m_captureWorker, &CaptureWorker::frameIndexed, this,
            &VideoRefereeWindow::onFrameIndexed, Qt::QueuedConnection);
    connect(m_captureWorker, &CaptureWorker::fpsDetected, this,
            &VideoRefereeWindow::onFpsDetected, Qt::QueuedConnection);
    connect(m_captureWorker, &CaptureWorker::rotationDone, this,
            &VideoRefereeWindow::onRotationDone, Qt::QueuedConnection);
    connect(m_captureWorker, &CaptureWorker::pauseDone, this,
            &VideoRefereeWindow::onPauseDone, Qt::QueuedConnection);
    connect(m_captureWorker, &CaptureWorker::errorOccurred, this,
            &VideoRefereeWindow::onCaptureError, Qt::QueuedConnection);
    connect(m_captureWorker, &CaptureWorker::finished, m_captureThread,
            &QThread::quit);
    connect(m_captureThread, &QThread::finished, this,
            &VideoRefereeWindow::onCaptureThreadFinished, Qt::QueuedConnection);

    m_captureThread->start();
}

void VideoRefereeWindow::stopCapture() {
    if (!m_captureWorker || !m_captureThread) {
        return;
    }
    m_captureWorker->stop();
    m_captureThread->quit();
    m_captureThread->wait(5000);
    m_captureThread->deleteLater();
    m_captureThread = nullptr;
    m_captureWorker = nullptr;
}

// Async variant: signals the capture thread to stop and calls onDone on the
// UI thread once it has fully exited. Returns immediately — does not block.
void VideoRefereeWindow::stopCaptureAsync(std::function<void()> onDone) {
    if (!m_captureWorker || !m_captureThread) {
        if (onDone) {
            onDone();
        }
        return;
    }
    m_pendingAfterStop = onDone;
    m_captureWorker->stop();
    m_captureThread->quit();
    // onCaptureThreadFinished() fires when the thread exits
}

void VideoRefereeWindow::onCaptureThreadFinished() {
    if (m_captureThread) {
        m_captureThread->deleteLater();
        m_captureThread = nullptr;
    }
    m_captureWorker = nullptr;

    broadcastStatus();  // sends # STATUS IDLE to all TCP clients

    if (m_pendingAfterStop) {
        auto cb = m_pendingAfterStop;
        m_pendingAfterStop = nullptr;
        cb();
    }
}

// ---- F1: request file rotation ----
void VideoRefereeWindow::onRotateFiles() {
    if (!m_captureWorker) {
        return;
    }

    QString newPath = newRecordingPath();

    // Close replay file if it's pointing at the current recording
    if (m_replayFile) {
        fclose(m_replayFile);
        m_replayFile = nullptr;
    }

    // Reset frame index — it will rebuild for the new file
    {
        QMutexLocker lk(&m_indexMutex);
        m_frameIndex.clear();
    }

    // If we were in replay, go back to live
    if (!m_isLive) {
        goLive();
    }

    m_captureWorker->requestRotate(newPath);
    m_currentRecordingPath = newPath;

    m_statusBar->showMessage("⟳ Rotating recording file…");
}

// ---- Capture thread finished rotating ----
void VideoRefereeWindow::onRotationDone(const QString& closedPath) {
    m_statusBar->showMessage(
        QString("✔ New file started. Queuing transcode of %1")
            .arg(QFileInfo(closedPath).fileName()));

    spawnTranscode(closedPath);
}

// ---- Capture paused (STOPCAPTURE): transcode closed file ----
void VideoRefereeWindow::onPauseDone(const QString& closedPath) {
    m_statusBar->showMessage(
        QString("Recording paused. Queuing transcode of %1")
            .arg(QFileInfo(closedPath).fileName()));
    spawnTranscode(closedPath);
    broadcastStatus();
}

// ---- Spawn ffmpeg as detached background process ----
void VideoRefereeWindow::spawnTranscode(const QString& sourcePath) {
    // Output: same name, .mp4 extension
    QString outPath = sourcePath;
    if (outPath.endsWith(".mjpeg")) {
        outPath.chop(6);
    }
    outPath += ".mp4";

    // Log file alongside the source
    QString logPath = sourcePath + ".transcode.log";

    // Batch script written to a temp file:
    //   ffmpeg ... && del source
    // On failure ffmpeg exits non-zero so the source is kept.
    // Batch files require backslashes on Windows
    auto winPath = [](const QString& p) { return QDir::toNativeSeparators(p); };

    QString batPath = sourcePath + ".transcode.bat";
    {
        QFile bat(batPath);
        if (bat.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream s(&bat);
            s << "@echo off\r\n";
            s << QString(
                     "ffmpeg -f mjpeg -framerate 50 -i \"%1\" "
                     "-c:v libx265 -crf 23 -preset fast -r 50 \"%2\" "
                     "> \"%3\" 2>&1\r\n")
                     .arg(winPath(sourcePath))
                     .arg(winPath(outPath))
                     .arg(winPath(logPath));
            s << "if %ERRORLEVEL% EQU 0 (\r\n";
            s << QString("    del \"%1\"\r\n").arg(winPath(sourcePath));
            s << QString("    del \"%1\"\r\n").arg(winPath(batPath));
            s << ") else (\r\n";
            s << QString("    echo ffmpeg failed, keeping source >> \"%1\"\r\n")
                     .arg(winPath(logPath));
            s << ")\r\n";
        }
    }

    QProcess* proc = new QProcess(this);

    // Track active transcodes so we can show count in status bar
    m_transcodeProcesses.append(proc);

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &VideoRefereeWindow::onTranscodeFinished);

    proc->setProperty("sourcePath", sourcePath);
    proc->setProperty("outPath", outPath);

#ifdef _WIN32
    proc->start("cmd.exe", QStringList() << "/c" << batPath);
#else
    // On Linux/Mac: run ffmpeg directly, delete on success
    proc->start("sh",
                QStringList()
                    << "-c"
                    << QString("ffmpeg -f mjpeg -framerate 50 -i '%1' "
                               "-c:v libx265 -crf 23 -preset fast -r 50 '%2' "
                               "> '%3' 2>&1 && rm '%1'")
                           .arg(sourcePath)
                           .arg(outPath)
                           .arg(logPath));
#endif

    updateStatusBar();
}

void VideoRefereeWindow::onTranscodeFinished(int exitCode,
                                             QProcess::ExitStatus /*status*/) {
    QProcess* proc = qobject_cast<QProcess*>(sender());
    if (!proc) {
        return;
    }

    QString src = proc->property("sourcePath").toString();
    QString out = proc->property("outPath").toString();

    m_transcodeProcesses.removeAll(proc);
    proc->deleteLater();

    if (m_closingDown) {
        if (m_shutdownDialog) {
            QLabel* label = m_shutdownDialog->findChild<QLabel*>();
            if (label) {
                if (exitCode == 0) {
                    label->setText(QString("Done: %1\nClosing...")
                                       .arg(QFileInfo(out).fileName()));
                } else {
                    label->setText(
                        QString("Transcode failed - source .mjpeg kept.\n%1")
                            .arg(QFileInfo(src).fileName()));
                }
            }
        }
        if (m_transcodeProcesses.isEmpty()) {
            if (m_shutdownDialog) {
                m_shutdownDialog->close();
                m_shutdownDialog = nullptr;
            }
            hide();
            QApplication::quit();
        }
        return;
    }

    if (exitCode == 0) {
        m_statusBar->showMessage(
            QString("Transcode complete: %1").arg(QFileInfo(out).fileName()));
    } else {
        m_statusBar->showMessage(
            QString("Transcode FAILED for %1 - source kept, check .log")
                .arg(QFileInfo(src).fileName()));
    }
    updateStatusBar();
}

// ---- Frame slots ----
void VideoRefereeWindow::onFrameCaptured(const QImage& image) {
    m_liveFrame = image;
    if (m_isLive) {
        showFrame(image, false);
        updateStatusBar();
    }
}

void VideoRefereeWindow::onFrameIndexed(qint64 filePos) {
    QMutexLocker lk(&m_indexMutex);
    m_frameIndex.push_back(filePos);
}

void VideoRefereeWindow::onFpsDetected(double fps) {
    m_captureFps = fps;
}

// ---- Read frame from current recording file ----
QImage VideoRefereeWindow::readFrameAt(qint64 filePos) {
    if (!m_replayFile) {
        m_replayFile =
            fopen(m_currentRecordingPath.toLocal8Bit().constData(), "rb");
        if (!m_replayFile) {
            return {};
        }
    }
    if (fseek(m_replayFile, static_cast<long>(filePos), SEEK_SET) != 0) {
        return {};
    }

    uint32_t magic = 0, lenNet = 0;
    if (fread(&magic, 4, 1, m_replayFile) != 1) {
        return {};
    }
    if (fread(&lenNet, 4, 1, m_replayFile) != 1) {
        return {};
    }
    if (fromBE32(magic) != kFrameMagic) {
        return {};
    }

    uint32_t len = fromBE32(lenNet);
    std::vector<uchar> buf(len);
    if (fread(buf.data(), 1, len, m_replayFile) != len) {
        return {};
    }

    cv::Mat decoded = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (decoded.empty()) {
        return {};
    }

    cv::Mat rgb;
    cv::cvtColor(decoded, rgb, cv::COLOR_BGR2RGB);
    return QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                  QImage::Format_RGB888)
        .copy();
}

// ---- Display ----
void VideoRefereeWindow::showFrame(const QImage& img, bool overlay) {
    if (img.isNull()) {
        return;
    }
    QImage display = img.scaled(m_display->size(), Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    if (overlay) {
        QPainter p(&display);
        QFont font("Monospace", 28, QFont::Bold);
        p.setFont(font);
        QRect tr =
            QFontMetrics(font).boundingRect("REPLAY").adjusted(-8, -4, 8, 4);
        tr.moveTopLeft({16, 16});
        p.fillRect(tr, QColor(0, 0, 0, 150));
        p.setPen(QColor(255, 50, 50));
        p.drawText(tr, Qt::AlignCenter, "REPLAY");

        font.setPointSize(16);
        p.setFont(font);
        p.setPen(QColor(255, 220, 0));
        p.drawText(16, 80,
                   m_isPaused
                       ? QString("⏸  PAUSED")
                       : QString("▶  %1×").arg(kSpeedSteps[m_speedIndex]));
    } else if (m_showMarker) {
        QPainter p(&display);
        QFont font("Monospace", 20, QFont::Bold);
        p.setFont(font);
        QRect mr = QFontMetrics(font)
                       .boundingRect("MARKER SET")
                       .adjusted(-8, -4, 8, 4);
        mr.moveTopLeft({16, 16});
        p.fillRect(mr, QColor(0, 160, 0, 200));
        p.setPen(Qt::white);
        p.drawText(mr, Qt::AlignCenter, "MARKER SET");
    }
    if (m_captureWorker != nullptr && m_captureWorker->isRecordingPaused()) {
        QPainter p(&display);
        QFont font("Monospace", 60, QFont::Bold);
        p.setFont(font);
        QRect mr = QFontMetrics(font)
                       .boundingRect("RECORDING PAUSED")
                       .adjusted(-8, -4, 8, 4);
        mr.moveCenter(display.rect().center());
        p.fillRect(mr, QColor(160, 0, 160, 200));
        p.setPen(Qt::white);
        p.drawText(mr, Qt::AlignCenter, "RECORDING PAUSED");
    }
    m_display->setPixmap(QPixmap::fromImage(display));
}

void VideoRefereeWindow::seekTo(int newFrame) {
    int total;
    {
        QMutexLocker lk(&m_indexMutex);
        total = static_cast<int>(m_frameIndex.size());
    }
    newFrame = qBound(0, newFrame, total - 1);
    m_replayFrame = newFrame;
    qint64 pos;
    {
        QMutexLocker lk(&m_indexMutex);
        pos = m_frameIndex[static_cast<size_t>(newFrame)];
    }
    showFrame(readFrameAt(pos), true);
    updateStatusBar();
}

void VideoRefereeWindow::onPlaybackTimer() {
    if (m_isLive || m_isPaused) {
        return;
    }
    int total;
    {
        QMutexLocker lk(&m_indexMutex);
        total = static_cast<int>(m_frameIndex.size());
    }
    if (++m_replayFrame >= total) {
        goLive();
        return;
    }
    qint64 pos;
    {
        QMutexLocker lk(&m_indexMutex);
        pos = m_frameIndex[static_cast<size_t>(m_replayFrame)];
    }
    showFrame(readFrameAt(pos), true);
    updateStatusBar();
}

void VideoRefereeWindow::enterReplay() {
    if (!m_isLive) {
        return;
    }
    m_isLive = false;
    {
        QMutexLocker lk(&m_indexMutex);
        m_replayFrame = static_cast<int>(m_frameIndex.size()) - 1;
    }
    updatePlaybackTimer();
    updateStatusBar();
        if (!m_liveFrame.isNull())
        showFrame(m_liveFrame, true);

}

void VideoRefereeWindow::goLive() {
    m_isLive = true;
    m_isPaused = false;
    m_speedIndex = kDefaultSpeedIdx;
    m_playbackTimer->stop();
    showFrame(m_liveFrame, false);
    updateStatusBar();
}

void VideoRefereeWindow::updatePlaybackTimer() {
    if (m_isLive || m_isPaused) {
        m_playbackTimer->stop();
        return;
    }
    int ms =
        static_cast<int>(1000.0 / (m_captureFps * kSpeedSteps[m_speedIndex]));
    m_playbackTimer->start(qMax(1, ms));
}

void VideoRefereeWindow::updateStatusBar() {
    int total;
    {
        QMutexLocker lk(&m_indexMutex);
        total = static_cast<int>(m_frameIndex.size());
    }
    double fps = m_captureFps > 0 ? m_captureFps : 50.0;
    int transcoding = m_transcodeProcesses.size();

    QString transcodeInfo =
        transcoding > 0
            ? QString("  |  ⚙ Transcoding: %1 file(s)").arg(transcoding)
            : QString();

    if (m_isLive) {
        m_statusBar->showMessage(
            QString("🔴 LIVE  cam%1  |  %2  |  Recorded: %3s (%4 frames)%5")
                .arg(m_cameraIndex)
                .arg(QFileInfo(m_currentRecordingPath).fileName())
                .arg(static_cast<int>(total / fps))
                .arg(total)
                .arg(transcodeInfo));
    } else {
        m_statusBar->showMessage(
            QString("⏮ REPLAY  |  %1s / %2s  (frame %3/%4)  |  Speed %5×  |  "
                    "Enter=Live  Space=Pause  ←→=±10s  ,/.=Speed  m//=±frame%6")
                .arg(static_cast<int>(m_replayFrame / fps))
                .arg(static_cast<int>(total / fps))
                .arg(m_replayFrame)
                .arg(total)
                .arg(kSpeedSteps[m_speedIndex])
                .arg(transcodeInfo));
    }
}

// ---- Keyboard ----
void VideoRefereeWindow::keyPressEvent(QKeyEvent* event) {
    double fps = m_captureFps > 0 ? m_captureFps : 50.0;
    int jump10s = static_cast<int>(fps * 10.0);

    switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            goLive();
            break;

        case Qt::Key_Space:
            if (m_isLive) {
                enterReplay();
                m_isPaused = true;
                m_playbackTimer->stop();
                seekTo(m_replayFrame);
            } else {
                m_isPaused = !m_isPaused;
                m_isPaused ? (m_playbackTimer->stop(), seekTo(m_replayFrame))
                           : updatePlaybackTimer();
            }
            updateStatusBar();
            break;

        case Qt::Key_Left:
            if (m_isLive) {
                enterReplay();
            }
            seekTo(m_replayFrame - jump10s);
            break;
        case Qt::Key_Right:
            if (m_isLive) {
                enterReplay();
            }
            seekTo(m_replayFrame + jump10s);
            break;

        case Qt::Key_Comma:
            if (m_isLive) {
                enterReplay();
            }
            m_speedIndex = qMax(0, m_speedIndex - 1);
            updatePlaybackTimer();
            updateStatusBar();
            break;

        case Qt::Key_Period:
            if (m_isLive) {
                enterReplay();
            }
            m_speedIndex = qMin(kNumSpeedSteps - 1, m_speedIndex + 1);
            updatePlaybackTimer();
            updateStatusBar();
            break;

        case Qt::Key_Slash:
            if (m_isLive) {
                enterReplay();
            }
            m_isPaused = true;
            m_playbackTimer->stop();
            seekTo(m_replayFrame + 1);
            break;

        case Qt::Key_M:
            if (m_isLive) {
                enterReplay();
            }
            m_isPaused = true;
            m_playbackTimer->stop();
            seekTo(m_replayFrame - 1);
            break;

        case Qt::Key_Z: {
            int total;
            {
                QMutexLocker lk(&m_indexMutex);
                total = static_cast<int>(m_frameIndex.size());
            }
            m_markedFrame = total;
            m_showMarker = true;
            m_markerTimer->start(1500);
            showFrame(m_isLive ? m_liveFrame
                               : readFrameAt(m_frameIndex[m_replayFrame]),
                      !m_isLive);
            break;
        }
        case Qt::Key_X:
            if (m_isLive) {
                enterReplay();
            }
            seekTo(m_markedFrame);
            break;

        default:
            QMainWindow::keyPressEvent(event);
            break;
    }
}

void VideoRefereeWindow::onCaptureError(const QString& msg) {
    QMessageBox::critical(this, "Capture Error", msg);
    m_statusBar->showMessage("Capture error: " + msg);
}

int VideoRefereeWindow::showCameraDialog(QWidget* parent, const QString& title,
                                         int currentIndex) {
    const auto devices = QMediaDevices::videoInputs();
    if (devices.isEmpty()) {
        QMessageBox::critical(parent, "No cameras found",
                              "No video capture devices were detected.");
        return -1;
    }

    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.setMinimumWidth(400);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel("Available cameras:", &dlg));

    QListWidget* list = new QListWidget(&dlg);
    for (int i = 0; i < devices.size(); ++i) {
        list->addItem(QString("Camera %1: %2%3")
                          .arg(i)
                          .arg(devices[i].description())
                          .arg(i == currentIndex ? " (current)" : ""));
    }
    list->setCurrentRow(qMax(0, currentIndex));
    layout->addWidget(list);

    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);

    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg,
                     &QDialog::reject);
    QObject::connect(list, &QListWidget::itemActivated, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) {
        return -1;
    }
    return list->currentRow();
}

void VideoRefereeWindow::onSwitchCamera() {
    int newIdx = showCameraDialog(this, "Switch Camera", m_cameraIndex);
    if (newIdx < 0 || newIdx == m_cameraIndex) {
        return;
    }

    stopCapture();
    goLive();
    if (m_replayFile) {
        fclose(m_replayFile);
        m_replayFile = nullptr;
    }
    {
        QMutexLocker lk(&m_indexMutex);
        m_frameIndex.clear();
    }
    startCapture(newIdx, newRecordingPath());
}

void VideoRefereeWindow::closeEvent(QCloseEvent* event) {
    if (m_closingDown) {
        // Called by hide() during shutdown — let it through
        event->accept();
        return;
    }

    m_closingDown = true;
    event->ignore();  // never accept here; we call QApplication::quit()
                      // ourselves

    // Stop capture — this flushes and closes the current .mjpeg file
    stopCapture();
    if (m_replayFile) {
        fclose(m_replayFile);
        m_replayFile = nullptr;
    }

    // Kill any mid-session transcodes that are still running (warm-up
    // files)
    for (QProcess* p : m_transcodeProcesses) {
        disconnect(p, nullptr, this, nullptr);
        p->kill();
        p->waitForFinished(2000);
        p->deleteLater();
    }
    m_transcodeProcesses.clear();

    // Transcode the final recording file
    spawnTranscode(m_currentRecordingPath);

    // Show a non-closeable "please wait" dialog
    m_shutdownDialog = new QDialog(
        nullptr, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    m_shutdownDialog->setWindowTitle("Transcoding final recording…");
    m_shutdownDialog->setFixedSize(420, 80);

    QLabel* label =
        new QLabel(QString("Transcoding %1\nPlease wait…")
                       .arg(QFileInfo(m_currentRecordingPath).fileName()),
                   m_shutdownDialog);
    label->setAlignment(Qt::AlignCenter);
    label->setGeometry(10, 10, 400, 60);

    m_shutdownDialog->show();
}

// ============================================================
//  TCP API
// ============================================================
void VideoRefereeWindow::initTcpServer() {
    m_tcpServer = new QTcpServer(this);
    connect(m_tcpServer, &QTcpServer::newConnection, this,
            &VideoRefereeWindow::onNewTcpConnection);

    if (!m_tcpServer->listen(QHostAddress::Any, 17801)) {
        m_statusBar->showMessage(
            QString("TCP API: failed to bind port 17801 - %1")
                .arg(m_tcpServer->errorString()));
    } else {
        m_statusBar->showMessage("TCP API: listening on port 17801");
    }
}

void VideoRefereeWindow::onNewTcpConnection() {
    while (m_tcpServer->hasPendingConnections()) {
        QTcpSocket* client = m_tcpServer->nextPendingConnection();
        m_tcpClients.append(client);

        connect(client, &QTcpSocket::readyRead, this,
                &VideoRefereeWindow::onTcpClientData);
        connect(client, &QTcpSocket::disconnected, this,
                &VideoRefereeWindow::onTcpClientDisconnected);

        // Send current status immediately on connect
        sendToClient(client, currentStatusLine());
    }
}

void VideoRefereeWindow::onTcpClientDisconnected() {
    QTcpSocket* client = qobject_cast<QTcpSocket*>(sender());
    if (!client) {
        return;
    }
    m_tcpClients.removeAll(client);
    client->deleteLater();
}

void VideoRefereeWindow::onTcpClientData() {
    QTcpSocket* client = qobject_cast<QTcpSocket*>(sender());
    if (!client) {
        return;
    }

    while (client->canReadLine()) {
        QString line = QString::fromUtf8(client->readLine()).trimmed();
        if (!line.isEmpty()) {
            handleTcpCommand(client, line.toUpper());
        }
    }
}

void VideoRefereeWindow::handleTcpCommand(QTcpSocket* client,
                                          const QString& cmd) {
    if (cmd == "ROTATE") {
        if (m_closingDown) {
            sendToClient(client, "< ERROR closing down");
            return;
        }
        onRotateFiles();
        sendToClient(client, kOk);

    } else if (cmd == "LIVE") {
        goLive();
        sendToClient(client, kOk);
        broadcastStatus();

    } else if (cmd == "PAUSE") {
        if (m_isLive) {
            enterReplay();
            m_isPaused = true;
            m_playbackTimer->stop();
            seekTo(m_replayFrame);
        } else {
            m_isPaused = !m_isPaused;
            m_isPaused ? (m_playbackTimer->stop(), seekTo(m_replayFrame))
                       : updatePlaybackTimer();
        }
        sendToClient(client, kOk);
        updateStatusBar();
        broadcastStatus();
    } else if (cmd == "SPEED +") {
        if (m_isLive) {
            enterReplay();
        }
        m_speedIndex = qMin(kNumSpeedSteps - 1, m_speedIndex + 1);
        updatePlaybackTimer();
        updateStatusBar();
        sendToClient(client, kOk);
        broadcastStatus();

    } else if (cmd == "SPEED -") {
        if (m_isLive) {
            enterReplay();
        }
        m_speedIndex = qMax(0, m_speedIndex - 1);
        updatePlaybackTimer();
        updateStatusBar();
        sendToClient(client, kOk);
        broadcastStatus();

    } else if (cmd.startsWith("SPEED ")) {
        bool ok = false;
        int step = cmd.mid(6).toInt(&ok);
        if (!ok || step < 1 || step > kNumSpeedSteps) {
            sendToClient(
                client,
                QString("< ERROR speed must be 1-%1").arg(kNumSpeedSteps));
            return;
        }
        if (m_isLive) {
            enterReplay();
        }
        m_speedIndex = step - 1;
        updatePlaybackTimer();
        updateStatusBar();
        sendToClient(client, kOk);
        broadcastStatus();

    } else if (cmd.startsWith("JUMP ")) {
        bool ok = false;
        int frames = cmd.mid(5).toInt(&ok);  // handles +N and -N
        if (!ok) {
            sendToClient(client, "< ERROR usage: JUMP +N or JUMP -N");
            return;
        }
        if (m_isLive) {
            enterReplay();
        }
        seekTo(m_replayFrame + frames);
        sendToClient(client, kOk);
        broadcastStatus();
    } else if (cmd == "STATUS") {
        sendToClient(client, currentStatusLine());
    } else if (cmd == "MARK") {
        int total;
        {
            QMutexLocker lk(&m_indexMutex);
            total = static_cast<int>(m_frameIndex.size());
        }
        m_markedFrame = total;
        m_showMarker = true;
        m_markerTimer->start(1500);
        showFrame(
            m_isLive ? m_liveFrame : readFrameAt(m_frameIndex[m_replayFrame]),
            !m_isLive);
        sendToClient(client, kOk);
    } else if (cmd == "RECALL") {
        if (m_isLive) {
            enterReplay();
        }
        seekTo(m_markedFrame);
        sendToClient(client, kOk);
        broadcastStatus();
    } else if (cmd == "STOPCAPTURE") {
        if (!m_captureWorker) {
            sendToClient(client, "< ERROR not running");
            return;
        }
        if (m_closingDown) {
            sendToClient(client, "< ERROR closing down");
            return;
        }
        if (m_captureWorker->isRecordingPaused()) {
            sendToClient(client, "< ERROR already paused");
            return;
        }
        goLive();
        if (m_replayFile) {
            fclose(m_replayFile);
            m_replayFile = nullptr;
        }
        {
            QMutexLocker lk(&m_indexMutex);
            m_frameIndex.clear();
        }
        m_captureWorker->pauseRecording();
        sendToClient(client, kOk);
        // onPauseDone fires when file is closed; status broadcast happens
        // there
    } else if (cmd == "STARTCAPTURE") {
        if (m_closingDown) {
            sendToClient(client, "< ERROR closing down");
            return;
        }
        if (!m_captureWorker) {
            sendToClient(client, "< ERROR capture not running");
            return;
        }
        if (!m_captureWorker->isRecordingPaused()) {
            sendToClient(client, "< ERROR not paused");
            return;
        }
        QString path = newRecordingPath();
        m_currentRecordingPath = path;
        {
            QMutexLocker lk(&m_indexMutex);
            m_frameIndex.clear();
        }
        m_captureWorker->resumeRecording(path);
        sendToClient(client, kOk);
        broadcastStatus();
    } else {
        sendToClient(client, "< ERROR unknown command");
    }
}

void VideoRefereeWindow::sendToClient(QTcpSocket* client, const QString& line) {
    if (!client || client->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    client->write((line + "\n").toUtf8());
}

void VideoRefereeWindow::broadcastStatus() {
    if (m_tcpClients.isEmpty()) {
        return;
    }
    QString line = currentStatusLine();
    for (QTcpSocket* client : m_tcpClients) {
        sendToClient(client, line);
    }
}

QString VideoRefereeWindow::currentStatusLine() const {
    int total;
    {
        QMutexLocker lk(&const_cast<QMutex&>(m_indexMutex));
        total = static_cast<int>(m_frameIndex.size());
    }
    double fps = m_captureFps > 0 ? m_captureFps : 50.0;
    int transcoding = m_transcodeProcesses.size();
    int recSecs = static_cast<int>(total / fps);

    auto toHMS = [](int secs) {
        return QString("%1:%2:%3")
            .arg(secs / 3600, 2, 10, QChar('0'))
            .arg((secs % 3600) / 60, 2, 10, QChar('0'))
            .arg(secs % 60, 2, 10, QChar('0'));
    };

    if (m_closingDown) {
        return QString("# STATUS CLOSING transcoding=%1").arg(transcoding);
    } else if (m_captureWorker && m_captureWorker->isRecordingPaused()) {
        return QString("# STATUS IDLE transcoding=%1").arg(transcoding);
    } else if (m_isLive) {
        return QString("# STATUS LIVE rec=%1 cam=%2 transcoding=%3")
            .arg(toHMS(recSecs))
            .arg(m_cameraIndex)
            .arg(transcoding);
    } else {
        int posSecs = static_cast<int>(m_replayFrame / fps);
        return QString(
                   "# STATUS REPLAY pos=%1 dur=%2 speed=%3 paused=%4 "
                   "transcoding=%5")
            .arg(toHMS(posSecs))
            .arg(toHMS(recSecs))
            .arg(kSpeedSteps[m_speedIndex])
            .arg(m_isPaused ? 1 : 0)
            .arg(transcoding);
    }
}

void VideoRefereeWindow::onStatusTimer() {
    broadcastStatus();
}

// ============================================================
//  main
// ============================================================
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("VideoReferee");

    int camIndex = VideoRefereeWindow::selectCamera();
    if (camIndex < 0) {
        return 0;
    }

    VideoRefereeWindow w(camIndex);
    w.show();
    return app.exec();
}

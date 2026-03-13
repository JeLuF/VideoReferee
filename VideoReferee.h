#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QLabel>
#include <QStatusBar>
#include <QThread>
#include <QMutex>
#include <QProcess>
#include <QDialog>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMediaDevices>
#include <QCameraDevice>

#include <opencv2/opencv.hpp>

#include <atomic>
#include <vector>

// ---------------------------------------------------------------------------
// CaptureWorker – runs in its own thread, writes DEADBEEF MJPEG file
// ---------------------------------------------------------------------------
class CaptureWorker : public QObject
{
    Q_OBJECT
public:
    explicit CaptureWorker(const QString &outputPath, int cameraIndex,
                           QObject *parent = nullptr);
    ~CaptureWorker();

    // Called from UI thread — capture loop will rotate at next frame boundary
    void requestRotate(const QString &newOutputPath);

public slots:
    void start();
    void stop();

signals:
    void frameCaptured(const QImage &image);   // scaled preview for display
    void frameIndexed(qint64 filePos);         // one frame written to disk
    void fpsDetected(double fps);
    void rotationDone(const QString &closedPath); // old file is closed & complete
    void finished();
    void errorOccurred(const QString &message);

private:
    QString           m_outputPath;
    int               m_cameraIndex;
    std::atomic<bool> m_running{false};

    // Rotation request: set atomically from UI thread, consumed by capture thread
    QMutex  m_rotateMutex;
    QString m_newOutputPath;   // non-empty = rotation pending
};

// ---------------------------------------------------------------------------
// VideoRefereeWindow
// ---------------------------------------------------------------------------
class VideoRefereeWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit VideoRefereeWindow(int cameraIndex, QWidget *parent = nullptr);
    ~VideoRefereeWindow();

    static int selectCamera();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onFrameCaptured(const QImage &image);
    void onFrameIndexed(qint64 filePos);
    void onFpsDetected(double fps);
    void onRotationDone(const QString &closedPath);
    void onTranscodeFinished(int exitCode, QProcess::ExitStatus status);
    void onPlaybackTimer();
    void onCaptureError(const QString &message);
    void onSwitchCamera();
    void onRotateFiles();   // F1 handler

    // ---- TCP API ----
    void onNewTcpConnection();
    void onTcpClientData();
    void onTcpClientDisconnected();
    void onStatusTimer();

private:
    // ---- UI ----
    QLabel      *m_display;
    QStatusBar  *m_statusBar;

    // ---- Capture ----
    QThread        *m_captureThread = nullptr;
    CaptureWorker  *m_captureWorker = nullptr;
    int             m_cameraIndex;
    QString         m_recordingDir;
    QString         m_currentRecordingPath;

    void startCapture(int cameraIndex, const QString &outputPath);
    void stopCapture();
    QString newRecordingPath() const;

    // ---- Frame index for current file ----
    QMutex              m_indexMutex;
    std::vector<qint64> m_frameIndex;

    // ---- Playback ----
    bool    m_isLive      = true;
    bool    m_isPaused    = false;
    bool    m_showMarker  = false;
    int     m_replayFrame = 0;
    int     m_markedFrame = 0;
    static const double kSpeedSteps[];
    int     m_speedIndex  = 3;

    QTimer  *m_playbackTimer;
    QTimer  *m_markerTimer;

    // ---- Replay file ----
    FILE    *m_replayFile = nullptr;

    // ---- Live frame ----
    QImage  m_liveFrame;
    double  m_captureFps = 50.0;

    // ---- Active transcodes (path → QProcess*) ----
    QList<QProcess*> m_transcodeProcesses;

    // ---- Shutdown state ----
    bool     m_closingDown = false;
    QDialog *m_shutdownDialog = nullptr;

    // ---- TCP API ----
    QTcpServer          *m_tcpServer  = nullptr;
    QList<QTcpSocket*>   m_tcpClients;
    QTimer              *m_statusTimer = nullptr;

    void initTcpServer();
    void handleTcpCommand(QTcpSocket *client, const QString &cmd);
    void sendToClient(QTcpSocket *client, const QString &line);
    void broadcastStatus();
    QString currentStatusLine() const;

    // ---- Helpers ----
    QImage  readFrameAt(qint64 filePos);
    void    spawnTranscode(const QString &sourcePath);
    void    enterReplay();
    void    goLive();
    void    showFrame(const QImage &img, bool replayOverlay);
    void    updatePlaybackTimer();
    void    updateStatusBar();
    void    seekTo(int newFrame);
};
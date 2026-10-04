// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QPointer>
#include <QProcess>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QImage>
#include <QUuid>
#include <QWidget>
#include <memory>

class QLabel;
class QProgressBar;
class QPushButton;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QTemporaryDir;
class QTimer;
class QLineEdit;
class QSlider;
class QTabBar;
class QShowEvent;
class QHideEvent;
class QVBoxLayout;
class QFormLayout;
class TimelineItemModel;
class KdenliveDoc;
namespace Mlt { class Producer; }

class StudioSubtitlePage final : public QWidget
{
public:
    explicit StudioSubtitlePage(QWidget *parent = nullptr);
    ~StudioSubtitlePage() override;
    void activate();
    void deactivate();
    void refreshSelection();
    QImage previewImage() const { return m_previewImage; }
    QString previewAssPath() const { return m_previewAss; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    enum class Stage { Idle, Extract, Recognize, Restyle };
    Stage m_stage = Stage::Idle;
    QProcess *m_process;
    QTimer *m_extractTimeout;
    QTimer *m_previewTimer;
    QTimer *m_previewUpdate;
    std::unique_ptr<QTemporaryDir> m_previewFolder;
    std::unique_ptr<Mlt::Producer> m_previewProducer;
    QString m_previewAss;
    QByteArray m_previewSignature;
    QImage m_previewImage;
    int m_renderCount = 0;
    std::unique_ptr<QTemporaryDir> m_temporary;
    std::shared_ptr<TimelineItemModel> m_timeline;
    KdenliveDoc *m_document = nullptr;
    QUuid m_sequence;
    QString m_binId;
    QString m_revision;
    int m_clipId = -1;
    int m_startFrame = 0;
    int m_playtime = 0;
    QByteArray m_output;
    QByteArray m_stderr;
    QByteArray m_extractLog;
    QByteArray m_restyleSnapshot;
    QJsonObject m_restyleStyle;
    int m_restyleWidth = 0;
    bool m_restyleWholeTrack = false;
    int m_restyleUndoIndex = -1;
    int m_restyleWithoutMarks = 0;
    int m_restyleStaleMarks = 0;
    QString m_workerError;
    QLabel *m_target;
    QLabel *m_status;
    QJsonObject m_style;
    QJsonArray m_presets;
    QJsonArray m_parameters;
    QHash<QString, QWidget *> m_styleControls;
    QFormLayout *m_appearanceForm = nullptr;
    bool m_schemaReady = false;
    QSpinBox *m_width;
    QComboBox *m_preset;
    QTabBar *m_styleTabs;
    QComboBox *m_customList;
    QJsonArray m_customStyles;
    QLineEdit *m_previewText;
    QLabel *m_preview;
    QSlider *m_seek;
    QPushButton *m_play;
    QPushButton *m_main;
    QPushButton *m_edit;
    QPushButton *m_restyle;
    QCheckBox *m_restyleAll;
    QPushButton *m_cancel;
    QProgressBar *m_progress;

    bool selectedClip(QString *error = nullptr);
    QString revisionToken() const;
    bool unchangedJob() const;
    bool writeAudioSnapshot(const QString &path, QString *error) const;
    void openEditor();
    void restyle();
    void finishRestyle();
    void start();
    void runRecognition();
    void finished(int exitCode, QProcess::ExitStatus status);
    void setBusy(bool busy);
    void setupControls(QVBoxLayout *layout);
    void applyStyle(const QJsonObject &values);
    void updatePreview();
    void renderPreview();
    void displayPreview();
    void stopPreview();
    void loadCustomStyles();
    void saveCustomStyles();
};

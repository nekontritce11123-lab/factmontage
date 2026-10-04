// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "parameters.hpp"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <memory>

class QLabel;
class QComboBox;
class QProgressBar;
class QPushButton;
class QSlider;
class QTabBar;
class QTemporaryDir;
class TimelineModel;
class TimelineItemModel;

class StudioAudioController final : public QObject
{
    Q_OBJECT
public:
    enum class Action { Voice, Loudness, Music, Pauses };
    struct Settings {
        int strength = studio_audio::defaults::strength;
        int presence = studio_audio::defaults::presence;
        int priority = studio_audio::defaults::priority;
        double targetLufs = studio_audio::defaults::target_lufs;
        QString noise = QString::fromLatin1(studio_audio::defaults::noise);
        bool levelTrack = false;
    };

    explicit StudioAudioController(QObject *parent = nullptr);
    ~StudioAudioController() override;
    QString selectionSummary(bool wholeTrack = false) const;
    bool assignCurrentSelection(bool voice, QString *error = nullptr);
    QString roleSummary() const;
    bool start(Action action, const Settings &settings, bool wholeTrack = false, QString *error = nullptr);
    QString recoverySummary() const;
    bool recover(QString *error = nullptr);
    void cancel();
    void selectionChanged();
    bool busy() const;

Q_SIGNALS:
    void statusChanged(const QString &text, bool error);
    void progressChanged(int value);
    void busyChanged(bool busy);
    void rolesChanged();

private:
    friend struct StudioAudioTests;
    struct Job;
    std::unique_ptr<Job> m_job;
    QSet<int> m_voiceIds;
    QSet<int> m_musicIds;
    QProcess *m_process = nullptr;
    QByteArray m_processOutput;
    QByteArray m_progressOutput;
    bool m_canceling = false;

    std::shared_ptr<TimelineItemModel> timeline() const;
    QSet<int> currentAudioSelection(QString *error = nullptr) const;
    static QSet<int> audioSelection(const std::shared_ptr<TimelineModel> &model, QString *error);
    static QSet<int> audioTrackSelection(const std::shared_ptr<TimelineModel> &model, QString *error);
    QString revisionToken(const std::shared_ptr<TimelineModel> &model, const QSet<int> &ids) const;
    bool startWithSources(Action action, const Settings &settings, const QSet<int> &voice, const QSet<int> &music, QString *error);
    bool writeStem(const std::shared_ptr<TimelineModel> &model, const QSet<int> &ids, int startFrame, int endFrame,
                   const QString &path, QString *error) const;
    void runNext();
    void processFinished(int exitCode, QProcess::ExitStatus status);
    bool applyResult(QString *error);
    bool applyPauseCuts(const QJsonObject &report, QString *error);
    bool applyAudioFile(const QJsonObject &report, QString *error);
    void fail(const QString &message);
    void finish(const QString &message);
    void discardOutput();
};

class StudioAudioPage final : public QWidget
{
    Q_OBJECT
public:
    explicit StudioAudioPage(StudioAudioController *controller, QWidget *parent = nullptr);
    void activate();
    void deactivate();
    void refreshSelection();

private:
    StudioAudioController *m_controller;
    QJsonObject m_schema;
    bool m_schemaValid = false;
    QTabBar *m_actions;
    QTabBar *m_presets;
    QComboBox *m_preset;
    QComboBox *m_noise;
    QComboBox *m_target;
    QComboBox *m_scope;
    QSlider *m_strength;
    QSlider *m_presence;
    QSlider *m_priority;
    QLabel *m_strengthValue;
    QLabel *m_presenceValue;
    QLabel *m_priorityValue;
    QLabel *m_roles;
    QLabel *m_hint;
    QLabel *m_status;
    QWidget *m_voiceOptions;
    QWidget *m_musicOptions;
    QWidget *m_loudnessOptions;
    QPushButton *m_voiceRole;
    QPushButton *m_musicRole;
    QPushButton *m_main;
    QPushButton *m_cancel;
    QPushButton *m_savePreset;
    QPushButton *m_recover;
    QProgressBar *m_progress;

    StudioAudioController::Action action() const;
    StudioAudioController::Settings settings() const;
    void updateMode();
    void applyPreset(int index);
    void savePreset();
};

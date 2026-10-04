// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QImage>
#include <QColor>
#include <QMap>
#include <QPointer>
#include <QRectF>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <memory>

class EffectStackModel;
class DocUndoStack;
class AssetParameterModel;
class QAction;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QMovie;
class QLineEdit;
class QProcess;
class QPushButton;
class QScrollArea;
class QSlider;
class QTabBar;
class QTimer;
class QToolButton;
class QVBoxLayout;
class StudioAudioController;
class StudioAudioPage;
class StudioSubtitlePage;
class StudioTextPage;

class StudioPanel : public QWidget
{
public:
    explicit StudioPanel(QWidget *parent = nullptr);
    ~StudioPanel() override;
    void refreshSelection();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct Control {
        QJsonObject spec;
        QWidget *row = nullptr;
        QLabel *label = nullptr;
        QLabel *error = nullptr;
        QList<QToolButton *> choices;
        QComboBox *combo = nullptr;
        QCheckBox *check = nullptr;
        QSlider *slider = nullptr;
        QDoubleSpinBox *spin = nullptr;
        QLineEdit *line = nullptr;
    };
    struct Group {
        QJsonObject spec;
        QWidget *section = nullptr;
        QToolButton *header = nullptr;
        QWidget *body = nullptr;
    };
    QMap<QString, Control> m_controls;
    QMap<QString, Group> m_groups;
    QJsonArray m_groupsSpec;
    QString m_resources;
    QString m_assetId;
    QString m_title;
    QLabel *m_target;
    QLabel *m_status;
    QLabel *m_scopeNote;
    QPushButton *m_add;
    QComboBox *m_instances;
    QToolButton *m_enabled;
    QToolButton *m_cards;
    QToolButton *m_camera;
    QToolButton *m_background;
    QToolButton *m_transitions;
    QToolButton *m_effectsNav;
    QToolButton *m_color;
    QToolButton *m_audio;
    QToolButton *m_subtitles;
    QTabBar *m_cameraTabs;
    QTabBar *m_backgroundTabs;
    QTabBar *m_effectTabs;
    QTabBar *m_textTabs;
    QLabel *m_pageTitle;
    QLabel *m_pageSubtitle;
    QToolButton *m_clipTarget;
    QToolButton *m_sequenceTarget;
    QTabBar *m_presetTabs;
    QWidget *m_presetBar;
    QComboBox *m_presets;
    QPushButton *m_savePreset;
    QAction *m_replacePreset;
    QAction *m_renamePreset;
    QAction *m_deletePreset;
    QPushButton *m_applyPreset;
    QPushButton *m_batchApply;
    QAction *m_fixOrder;
    QAction *m_duplicateEffect;
    QAction *m_removeTransition;
    QAction *m_backgroundQualityMenuAction;
    QWidget *m_content;
    QVBoxLayout *m_contentLayout;
    QScrollArea *m_scroll;
    QMovie *m_movie;
    QTimer *m_hoverTimer;
    QPointer<QToolButton> m_preview;
    QPointer<QToolButton> m_hoverPreview;
    QWidget *m_effectFilters = nullptr;
    QLineEdit *m_effectSearch = nullptr;
    QComboBox *m_effectCategory = nullptr;
    QComboBox *m_effectFilter = nullptr;
    QToolButton *m_effectFavorite = nullptr;
    QJsonArray m_effectRecipes;
    std::shared_ptr<EffectStackModel> m_stack;
    std::shared_ptr<AssetParameterModel> m_effect;
    QList<std::shared_ptr<AssetParameterModel>> m_effects;
    QList<QMetaObject::Connection> m_connections;
    QMetaObject::Connection m_parameterConnection;
    QMetaObject::Connection m_monitorConnection;
    QMetaObject::Connection m_monitorKeyConnection;
    QPushButton *m_framing = nullptr;
    QPushButton *m_framingDone = nullptr;
    QPushButton *m_cardPosition = nullptr;
    QPushButton *m_cardPositionDone = nullptr;
    QPushButton *m_effectCenter = nullptr;
    QPushButton *m_effectCenterDone = nullptr;
    QPushButton *m_center = nullptr;
    QPushButton *m_swap = nullptr;
    QPushButton *m_pointStart = nullptr;
    QPushButton *m_pointEnd = nullptr;
    QLabel *m_trackingFrame = nullptr;
    QPushButton *m_trackingApply = nullptr;
    QPushButton *m_headCancel = nullptr;
    std::shared_ptr<AssetParameterModel> m_dragEffect;
    std::weak_ptr<DocUndoStack> m_dragUndo;
    QStringList m_dragNames, m_dragBefore, m_dragAfter;
    bool m_loaded = false;
    bool m_sequence = false;
    int m_page = 0;
    bool m_framingActive = false;
    bool m_framingClipMonitor = false;
    bool m_cardPositioning = false;
    bool m_effectCentering = false;
    bool m_editEnd = true;
    int m_selectedClipId = -1;
    int m_transitionFirst = -1;
    int m_transitionSecond = -1;
    int m_transitionSoundFirst = -1;
    int m_transitionSoundSecond = -1;
    const void *m_transitionSoundModel = nullptr;
    bool m_transitionPending = false;
    QString m_transitionSoundSource;
    QImage m_trackingImage;
    QPointF m_trackingPoint{-1, -1};
    double m_pendingTrackingZoom = 120.0;
    int m_pendingRecipe = 0;
    QProcess *m_tracker = nullptr;
    std::weak_ptr<AssetParameterModel> m_trackingEffect;
    std::weak_ptr<EffectStackModel> m_trackingStack;
    int m_trackingClipId = -1;
    QString m_trackingZoom;
    QString m_trackingOutput;
    QTimer *m_monitorCommitTimer;
    StudioAudioController *m_audioController = nullptr;
    QPointer<StudioAudioPage> m_audioPage;
    QPointer<StudioSubtitlePage> m_subtitlePage;
    QPointer<StudioTextPage> m_textPage;
    QHash<QString, QJsonObject> m_textDrafts;
    QWidget *m_backgroundBox = nullptr;
    QLabel *m_backgroundFrame = nullptr;
    QPushButton *m_backgroundColor = nullptr;
    QImage m_backgroundImage;
    QColor m_backgroundKey{0, 255, 0};
    QColor m_backgroundFill{31, 36, 48};
    int m_backgroundMethod = 0;
    QJsonArray m_backgroundPresets;
    QStringList m_backgroundParameters;
    QJsonObject m_pendingBackgroundValues;
    QJsonObject m_backgroundJobValues;
    int m_backgroundAnalysisSize = 768;
    QProcess *m_backgroundProducer = nullptr;
    QProcess *m_backgroundAnalyzer = nullptr;
    std::weak_ptr<EffectStackModel> m_backgroundStack;
    std::weak_ptr<AssetParameterModel> m_backgroundEffect;
    QString m_backgroundOutput;
    QString m_backgroundXml;
    QString m_backgroundRecipe;
    QString m_backgroundSource;
    QString m_backgroundAnalyzerOutput;
    QString m_backgroundProcessError;
    int m_backgroundClipId = -1;
    int m_backgroundClipIn = 0;
    int m_backgroundClipFrames = 0;
    bool m_backgroundCanceled = false;
    std::shared_ptr<AssetParameterModel> m_monitorEffect;
    QStringList m_monitorNames, m_monitorBefore, m_monitorAfter;

    void loadProduct(int index);
    void cancelAnalyses();
    bool supported() const;
    bool currentTarget() const;
    QWidget *makeControl(const QJsonObject &spec);
    void refreshValues();
    void selectInstance(int index);
    void commit(const QString &key, double value);
    void commitText(const QString &key, const QString &value);
    void finishDrag();
    void resetGroup(const QString &key);
    void startPreview(QToolButton *button);
    void stopPreview();
    void loadPresets();
    void savePreset(bool replace);
    void renamePreset();
    void deletePreset();
    void applySelectedPreset(bool batch);
    QMap<QString, QString> presetValues(const QString &path) const;
    void startFraming();
    void stopFraming();
    void updateFramingRect();
    void monitorRectChanged(const QRectF &rect);
    void finishMonitorGesture();
    void centerFraming();
    void swapFraming();
    void startCardPosition();
    void updateCardPositionRect();
    void loadTrackingFrame();
    void paintTrackingFrame();
    void runHeadTracking();
    void headTrackingFinished(int exitCode);
    void setBackgroundMethod(int method);
    bool backgroundNeedsAnalysis() const;
    void loadBackgroundFrame();
    void paintBackgroundFrame();
    void applyBackground();
    void runBackgroundAnalysis();
    void backgroundAnalysisFinished();
    void applyBackgroundPreset(const QJsonObject &preset);
    void resolveBackgroundAssets(int clipId = -1);
    QPointer<QObject> m_backgroundAssetDocument;
    QString m_backgroundAssetRoot;
    quint64 m_frameRequest = 0;
    void fixCameraOrder();
    void filterEffectRecipes();
    void selectEffectRecipe(int slot);
    void applyEffectRecipe(bool duplicate = false);
    QString effectRecipeName(int slot) const;
    void startEffectCenter();
    void updateEffectCenterRect();
    QString parameterName(const QJsonObject &spec) const;
    double displayedValue(const QJsonObject &spec) const;
    QString storedValue(const QJsonObject &spec, double value) const;
    QString choiceName(const QJsonObject &spec, int value) const;
    void refreshTransitionSelection();
    QVector<QPair<QString, QVariant>> transitionParameters(int frames) const;
    void chooseTransitionSound(const QString &source);
    bool applyTransition(int frames, QVector<QPair<QString, QVariant>> parameters, bool remove = false);
    void removeTransition();
};

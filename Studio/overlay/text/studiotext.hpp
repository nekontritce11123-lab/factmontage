// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QColor>
#include <QImage>
#include <QLibrary>
#include <QWidget>
#include <functional>
#include <memory>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTabBar;
class QTimer;
class TimelineItemModel;
class EffectItemModel;

namespace StudioText {
bool createTitle(const std::shared_ptr<TimelineItemModel> &timeline, int baseClip, int frames,
                 const QString &name, const QString &scene, const std::function<void(bool, const QString &)> &finished);
bool updateTitle(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, const std::shared_ptr<EffectItemModel> &effect,
                 int frames, const QString &scene);
}

class StudioTextPage final : public QWidget
{
public:
    explicit StudioTextPage(QHash<QString, QJsonObject> *drafts, QWidget *parent = nullptr);
    ~StudioTextPage() override;
    void refreshSelection();
    void deactivate();
    QImage previewImage() const { return m_previewImage; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QJsonArray m_catalog;
    QHash<QString, QJsonObject> *m_drafts;
    QString m_targetKey;
    int m_selectedClip = -1;
    int m_clipFrames = -1;
    QString m_scene;
    QString m_sceneError;
    QLabel *m_target;
    QLabel *m_status;
    QPlainTextEdit *m_text;
    QFontComboBox *m_font;
    QCheckBox *m_bold;
    QCheckBox *m_italic;
    QPushButton *m_color;
    QColor m_textColor = QColor(QStringLiteral("#fff9ef"));
    QCheckBox *m_outlineEnabled;
    QSpinBox *m_outlineWidth;
    QPushButton *m_outlineColor;
    QColor m_outlineColorValue = QColor(QStringLiteral("#ff000000"));
    QSpinBox *m_opacity;
    QSpinBox *m_shadow;
    QPushButton *m_shadowColorButton;
    QColor m_shadowColorValue = QColor(QStringLiteral("#80000000"));
    QCheckBox *m_backgroundEnabled;
    QPushButton *m_backgroundColorButton;
    QColor m_backgroundColorValue = QColor(QStringLiteral("#66000000"));
    QSpinBox *m_size;
    QSpinBox *m_width;
    QComboBox *m_alignment;
    QSpinBox *m_letterSpacing;
    QSpinBox *m_wordSpacing;
    QSpinBox *m_lineSpacing;
    QSpinBox *m_x;
    QSpinBox *m_y;
    QDoubleSpinBox *m_duration;
    QDoubleSpinBox *m_inDuration;
    QDoubleSpinBox *m_outDuration;
    QComboBox *m_style;
    QComboBox *m_entrance;
    QComboBox *m_life;
    QComboBox *m_exit;
    QComboBox *m_group;
    QComboBox *m_order;
    QSpinBox *m_lag;
    QSpinBox *m_motionAmount;
    QDoubleSpinBox *m_motionSpeed;
    QPushButton *m_apply;
    QTabBar *m_styleTabs;
    QComboBox *m_customList;
    QJsonArray m_customStyles;
    QLabel *m_preview;
    QSlider *m_seek;
    QPushButton *m_play;
    QTimer *m_previewTimer;
    QTimer *m_previewUpdate;
    QImage m_previewImage;
    QByteArray m_cachedScene;
    QByteArray m_loadedScene;
    QJsonObject m_cachedSettings;
    QJsonObject m_cachedRaster;
    QSize m_cachedSize;
    QLibrary m_nativeLibrary;
    bool m_nativeChecked = false;
    void *m_native = nullptr;
    void (*m_destroy)(void *) = nullptr;
    int (*m_load)(void *, const void *, size_t) = nullptr;
    int (*m_render)(void *, double, unsigned, unsigned, const void *, void *) = nullptr;
    const char *(*m_lastError)(void *) = nullptr;
    int m_renderCount = 0;

    QJsonObject settings() const;
    void loadSettings(const QJsonObject &settings);
    void updateApplyState();
    QString selectionKey(const std::shared_ptr<TimelineItemModel> &timeline, int clipId, const QString &scene) const;
    void apply();
    QByteArray compiledDraft(const QJsonObject &values, QSize size);
    void queuePreview();
    void renderPreview();
    void displayPreview();
    void stopPreview();
    void loadCustomStyles();
    void saveCustomStyles();
};

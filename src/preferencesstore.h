#ifndef PREFERENCESSTORE_H
#define PREFERENCESSTORE_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

class QSettings;

/**
 * @brief 外壳层的偏好设置存储
 *
 * 键名与 MarkText 的 packages/desktop/static/preference.json **完全一致**，
 * 默认值也照抄该文件，目的在于：
 *   1. 后续 WebView2 渲染层接入后，桥接层可以把同一份配置直接喂给渲染层，
 *      渲染层 `userPreference.getAll()` 拿到的键名/缺省值与 Electron 版一致；
 *   2. 偏好设置窗口（设置菜单）与菜单勾选状态共用同一数据源。
 *
 * 落盘位置：<exe 目录>/config/QtMarkText.ini（节 [preference]，UTF-8）
 * —— 与 MarkText 的 userData 目录无关，属外壳自有配置，便于整包携带。
 */
class PreferencesStore : public QObject
{
    Q_OBJECT

public:
    explicit PreferencesStore(QObject *parent = nullptr);
    ~PreferencesStore() override;

    /// 单例（外壳内共享一份配置）
    static PreferencesStore *instance();

    /// ini 文件绝对路径
    static QString iniFilePath();

    /// 读配置（读不到时回落到 MarkText 默认值）
    QVariant value(const QString &sKey) const;
    bool    boolValue(const QString &sKey) const;
    int     intValue(const QString &sKey) const;
    QString stringValue(const QString &sKey) const;

    /// 写配置（值等于 MarkText 默认值时仍会写入，便于用户看清当前状态）
    void setValue(const QString &sKey, const QVariant &valueValue);

    /// MarkText 的缺省值
    QVariant    defaultValue(const QString &sKey) const;
    /// 本外壳认识的配置键（= MarkText 默认表里的键）
    QStringList keys() const;
    /// 批量取出（供渲染层一次性同步）
    QVariantMap all() const;

    void sync();

signals:
    void preferenceChanged(const QString &sKey, const QVariant &valueValue);

private:
    static const QHash<QString, QVariant> &defaults();

    QSettings *m_pSettings;
};

#endif // PREFERENCESSTORE_H

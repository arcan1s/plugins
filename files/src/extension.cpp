// albert - a simple application launcher for linux
// Copyright (C) 2014-2015 Manuel Schneider
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "extension.h"
#include <QDebug>
#include <QList>
#include <QMessageBox>
#include <QMutex>
#include <QStandardPaths>
#include <QSettings>
#include <QThreadPool>
#include <QTimer>
#include "scanworker.h"
#include "configwidget.h"
#include "query.h"
#include "file.h"

namespace Files{


/** ***************************************************************************/
Extension::Extension() {
    _fileIndex = new QList<SharedFile>;
}



/** ***************************************************************************/
Extension::~Extension() {
    delete _fileIndex;
}



/** ***************************************************************************/
void Extension::initialize() {
    qDebug() << "[Files] Initialize extension";

    // Load settings
    QSettings s;
    s.beginGroup(CFG_GROUP);
    _indexOptions.indexAudio = s.value(CFG_INDEX_AUDIO, CFG_INDEX_AUDIO_DEF).toBool();
    _indexOptions.indexVideo = s.value(CFG_INDEX_VIDEO, CFG_INDEX_VIDEO_DEF).toBool();
    _indexOptions.indexImage = s.value(CFG_INDEX_IMAGE, CFG_INDEX_IMAGE_DEF).toBool();
    _indexOptions.indexDocs  = s.value(CFG_INDEX_DOC, CFG_INDEX_DOC_DEF).toBool();
    _indexOptions.indexDirs  = s.value(CFG_INDEX_DIR, CFG_INDEX_DIR_DEF).toBool();
    _indexOptions.indexHidden= s.value(CFG_INDEX_HIDDEN, CFG_INDEX_HIDDEN_DEF).toBool();
    _searchIndex.setFuzzy(s.value(CFG_FUZZY, CFG_FUZZY_DEF).toBool());

    // Load the paths or set a default
    QVariant v = s.value(CFG_PATHS);
    if (v.isValid() && v.canConvert(QMetaType::QStringList))
        _rootDirs = v.toStringList();
    else
        restorePaths();

    // scan interval timer
    connect(&_intervalTimer, &QTimer::timeout, this, &Extension::updateIndex);
    _intervalTimer.setInterval(s.value(CFG_SCAN_INTERVAL, CFG_SCAN_INTERVAL_DEF).toUInt()*60000);
    _intervalTimer.start();

    // Initial update
    updateIndex();

    s.endGroup();
}



/** ***************************************************************************/
void Extension::finalize() {
    qDebug() << "[Files] Finalize extension";

    // Save settings
    QSettings s;
    s.beginGroup(CFG_GROUP);
    s.setValue(CFG_FUZZY, _searchIndex.fuzzy());
    s.setValue(CFG_PATHS, _rootDirs);
    s.setValue(CFG_INDEX_AUDIO, _indexOptions.indexAudio);
    s.setValue(CFG_INDEX_VIDEO, _indexOptions.indexVideo);
    s.setValue(CFG_INDEX_IMAGE, _indexOptions.indexImage);
    s.setValue(CFG_INDEX_DIR, _indexOptions.indexDirs);
    s.setValue(CFG_INDEX_DOC, _indexOptions.indexDocs);
    s.setValue(CFG_INDEX_HIDDEN,_indexOptions.indexHidden);
    s.setValue(CFG_SCAN_INTERVAL,_intervalTimer.interval()/60000);
    s.endGroup();
}



/** ***************************************************************************/
QWidget *Extension::widget() {
    if (_widget.isNull()){
        _widget = new ConfigWidget;

        // Paths
        _widget->ui.listWidget_paths->addItems(_rootDirs);
        _widget->ui.label_info->setText(QString("%1 files indexed.").arg(_fileIndex->size()));
        connect(this, &Extension::rootDirsChanged, _widget->ui.listWidget_paths, &QListWidget::clear);
        connect(this, &Extension::rootDirsChanged, _widget->ui.listWidget_paths, &QListWidget::addItems);
        connect(_widget, &ConfigWidget::requestAddPath, this, &Extension::addDir);
        connect(_widget, &ConfigWidget::requestRemovePath, this, &Extension::removeDir);
        connect(_widget->ui.pushButton_restore, &QPushButton::clicked, this, &Extension::restorePaths);
        connect(_widget->ui.pushButton_update, &QPushButton::clicked, this, &Extension::updateIndex);

        // Checkboxes
        _widget->ui.checkBox_audio->setChecked(_indexOptions.indexAudio);
        connect(_widget->ui.checkBox_audio, &QCheckBox::toggled, this, &Extension::setIndexOptionAudio);

        _widget->ui.checkBox_video->setChecked(_indexOptions.indexVideo);
        connect(_widget->ui.checkBox_video, &QCheckBox::toggled, this, &Extension::setIndexOptionVideo);

        _widget->ui.checkBox_image->setChecked(_indexOptions.indexImage);
        connect(_widget->ui.checkBox_image, &QCheckBox::toggled, this, &Extension::setIndexOptionImage);

        _widget->ui.checkBox_docs->setChecked(_indexOptions.indexDocs);
        connect(_widget->ui.checkBox_docs, &QCheckBox::toggled, this, &Extension::setIndexOptionDocs);

        _widget->ui.checkBox_dirs->setChecked(_indexOptions.indexDirs);
        connect(_widget->ui.checkBox_dirs, &QCheckBox::toggled, this, &Extension::setIndexOptionDirs);

        _widget->ui.checkBox_hidden->setChecked(_indexOptions.indexHidden);
        connect(_widget->ui.checkBox_hidden, &QCheckBox::toggled, this, &Extension::setIndexOptionHidden);

        _widget->ui.checkBox_fuzzy->setChecked(_searchIndex.fuzzy());
        connect(_widget->ui.checkBox_fuzzy, &QCheckBox::toggled, this, &Extension::setFuzzy);

        _widget->ui.spinBox_interval->setValue(scanInterval());
        connect(_widget->ui.spinBox_interval, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, &Extension::setScanInterval);

        // Info
        connect(this, &Extension::statusInfo, _widget->ui.label_info, &QLabel::setText);
    }
    return _widget;
}



/** ***************************************************************************/
void Extension::teardownSession() {
    File::clearIconCache();
}



/** ***************************************************************************/
void Extension::handleQuery(Query *q) {
    _mutex.lock();
    q->addResults(_searchIndex.search(q->searchTerm()));
    _mutex.unlock();
}



/** ***************************************************************************/
void Extension::setFuzzy(bool b) {
    _mutex.lock();
    _searchIndex.setFuzzy(b);
    _mutex.unlock();
}



/** ***************************************************************************
 * @brief addDir
 * @param dirPath
 * @return 0 success, 1 does not exist, 2 is not a dir, 3 already watched,
 * 4 is sub dir of other root
 */
void Extension::addDir(const QString &dirPath) {
    qDebug() << "[Files] Adding dir" << dirPath;

    QFileInfo fileInfo(dirPath);

    // Get an absolute file path
    QString absPath = fileInfo.absoluteFilePath();

    // Check existance
    if (!fileInfo.exists()){
        QMessageBox(QMessageBox::Critical, "Error", absPath + " does not exist.").exec();
        return;
    }

    // Check type
    if(!fileInfo.isDir()){
        QMessageBox(QMessageBox::Critical, "Error", absPath + " is not a directory.").exec();
        return;
    }

    // Check if there is an identical existing path
    if (_rootDirs.contains(absPath)){
        QMessageBox(QMessageBox::Critical, "Error", absPath + " has already been indexed.").exec();
        return;
    }

    // Check if this dir is a subdir of an existing dir
    for (const QString &p: _rootDirs)
        if (absPath.startsWith(p + '/')){
            QMessageBox(QMessageBox::Critical, "Error", absPath + " is subdirectory of " + p).exec();
            return;
        }

    // Check if this dir is a superdir of an existing dir, in case delete subdir
    for (QStringList::iterator it = _rootDirs.begin(); it != _rootDirs.end();)
        if (it->startsWith(absPath + '/')){
            QMessageBox(QMessageBox::Warning, "Warning",
                        (*it) + " is subdirectory of " + absPath + ". " + (*it) + " will be removed.").exec();
            it = _rootDirs.erase(it);
        } else ++it;

    // Add the path to root dirs
    _rootDirs << absPath;

    // Inform observers
    emit rootDirsChanged(_rootDirs);
}



/** ***************************************************************************/
void Extension::removeDir(const QString &dirPath) {
    qDebug() << "[Files] Removing path" << dirPath;

    // Get an absolute file path
    QString absPath = QFileInfo(dirPath).absoluteFilePath();

    // Check existance
    if (!_rootDirs.contains(absPath))
        return;

    // Remove the path
    _rootDirs.removeAll(absPath);

    // Update the widget, if it is visible atm
    emit rootDirsChanged(_rootDirs);
}



/** ***************************************************************************/
void Extension::restorePaths() {
    qDebug() << "[Files] Restore paths to defaults";

    // Add standard paths
    _rootDirs.clear();
    addDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
}



/** ***************************************************************************/
void Extension::updateIndex() {
    qDebug() << "[Files] Updating index";

    // If thread is running, stop it and start this functoin after termination
    if (!_scanWorker.isNull()){
        _scanWorker->abort();
        connect(_scanWorker, &ScanWorker::destroyed, this, &Extension::updateIndex);
    } else {
        // Cretae a new scanning runnable for the threadpool
        _scanWorker = new ScanWorker(&_fileIndex, &_searchIndex, _rootDirs, _indexOptions, &_mutex);

        //  Run it
        QThreadPool::globalInstance()->start(_scanWorker);

        // Reset timer to full time
        _intervalTimer.start();

        // If widget is visible show the information in the status bat
        if (!_widget.isNull())
            connect(_scanWorker, &ScanWorker::statusInfo, _widget->ui.label_info, &QLabel::setText);
    }
}



/** ***************************************************************************/
void Extension::setScanInterval(uint minutes){
    if (minutes == 0)
        _intervalTimer.stop();
    else
        _intervalTimer.start(minutes*60000);
}
}
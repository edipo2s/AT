#include "untapped.h"
#include "../macros.h"

#include <QApplication>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>

#define TAR_PATH "bin/tar.exe"
#define XZ_PATH "bin/xz.exe"

Untapped::Untapped(QObject *parent) : QObject(parent)
{
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::DataLocation);
    tempDir = QFile(dataDir + QDir::separator() + "temp").fileName();
    QDir dir(tempDir);
    if (!dir.exists() || dir.isEmpty()) {
        QDir dir;
        dir.mkpath(tempDir);
    }

    untappedAPI = new UntappedAPI(this);
    setupUntappedAPIConnections();
}

void Untapped::setupUntappedAPIConnections()
{
    connect(untappedAPI, &UntappedAPI::sgnS3PutInfo, this, &Untapped::onS3PutInfo);
    connect(untappedAPI, &UntappedAPI::sgnNewAnonymousUploadToken,
            this, [](QString uploadToken){
        APP_SETTINGS->setUntappedAnonymousUploadToken(uploadToken);
    });
}

void Untapped::checkForUntappedUploadToken()
{
    if (APP_SETTINGS->getUntappedAnonymousUploadToken().isEmpty()) {
        untappedAPI->fetchAnonymousUploadToken();
    }
}

void Untapped::setEventPlayerCourse(EventPlayerCourse eventPlayerCourse)
{
    this->eventPlayerCourse = eventPlayerCourse;
}

void Untapped::uploadMatchToUntapped(MatchInfo matchInfo, QStack<QString> matchLogMsgs)
{
    this->matchInfo = matchInfo;
    prepareMatchLogFile(matchLogMsgs);
    untappedAPI->requestS3PutUrl();
}

void Untapped::prepareMatchLogFile(QStack<QString> matchLogMsgs)
{
    QFile logFile(tempDir + QDir::separator() + "log.txt");
    if (logFile.exists()) {
      logFile.remove();
    }
    logFile.open(QIODevice::WriteOnly | QIODevice::Text);
    logFile.seek(0);
    while(!matchLogMsgs.isEmpty()) {
        logFile.write(matchLogMsgs.pop().toUtf8());
    }
    logFile.flush();
    logFile.close();
    QProcess::execute(XZ_PATH, QStringList() << "-f" << logFile.fileName());
}

void Untapped::prepareMatchDescriptor(QString timestamp)
{
    QJsonDocument descriptor = untappedMatchDescriptor.prepareNewDescriptor(matchInfo, timestamp,
                                                                            eventPlayerCourse);
    QFile descriptorFile(tempDir + QDir::separator() + "descriptor.json");
    if (descriptorFile.exists()) {
      descriptorFile.remove();
    }
    descriptorFile.open(QIODevice::WriteOnly | QIODevice::Text);
    descriptorFile.write(descriptor.toJson());
    descriptorFile.flush();
    descriptorFile.close();
}

void Untapped::onS3PutInfo(QString putUrl, QString timestamp)
{
    if (matchInfo.games.isEmpty()) {
        influx_metric(influxdb_cpp::builder()
            .meas("lt_match_without_games")
            .tag("matchId", matchInfo.matchId.toStdString())
            .tag("event", matchInfo.eventId.toStdString())
            .field("count", 1)
        );
        return;
    }
    prepareMatchDescriptor(timestamp);
    preparePutPayloadFile();
    LOGI("Upload");
}

void Untapped::preparePutPayloadFile()
{
    QFile packedFile(tempDir + QDir::separator() + "match.packed");
    if (packedFile.exists()) {
      packedFile.remove();
    }
    QStringList args;
    args << "-cf";
    args << packedFile.fileName();
    args << "-C";
    args << tempDir;
    args << "descriptor.json";
    args << "log.txt.xz";
    QProcess::execute(TAR_PATH, args);
    QFile(tempDir + QDir::separator() + "descriptor.json").remove();
    QFile(tempDir + QDir::separator() + "log.txt.xz").remove();
}

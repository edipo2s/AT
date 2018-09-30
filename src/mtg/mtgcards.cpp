#include "mtgcards.h"
#include "../urls.h"
#include "../transformations.h"
#include "../macros.h"

#include <QByteArray>
#include <QDirIterator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonValueRef>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

MtgCards::MtgCards(QObject *parent) : QObject(parent)
{
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::DataLocation);
    if(RUNNING_TESTS){
        dataDir = ":res";
    }
    setsDir = dataDir + QDir::separator() + "sets";
    QDir dir(setsDir);
    if (!dir.exists() || dir.isEmpty()) {
        QDir dir;
        dir.mkpath(setsDir);
    }
    updateMtgaSetsFromAPI();
}

Card* MtgCards::findCard(int mtgaId)
{
    if (cards.keys().contains(mtgaId)) {
        return cards[mtgaId];
    } else {
        return new Card(mtgaId, 0, "", "", QString("UNKNOWN %1").arg(mtgaId));
    }
}

void MtgCards::updateMtgaSetsFromAPI(){
    LOGD(QString("Updating mtga sets"));
    QUrl url(QString("%1/sets").arg(URLs::API()));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (LOG_REQUEST_ENABLED) {
        LOGD(QString("Get Request: %1").arg(url.toString()));
    }
    QNetworkReply *reply = networkManager.get(request);
    connect(reply, &QNetworkReply::finished,
            this, &MtgCards::updateMtgaSetsFromAPIRequestOnFinish);
}

void MtgCards::updateMtgaSetsFromAPIRequestOnFinish()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(sender());
    QJsonObject jsonRsp = Transformations::stringToJsonObject(reply->readAll());
    if (LOG_REQUEST_ENABLED) {
        LOGD(QString(QJsonDocument(jsonRsp).toJson()));
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode < 200 || statusCode > 299) {
        QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        LOGW(QString("Error: %1 - %2").arg(reply->errorString()).arg(reason));
        LOTUS_TRACKER->showMessage(tr("Error downloading sets list"));
        return;
    }

    LOGD(QString("Mtga Sets downloaded. %1 sets.").arg(jsonRsp.size()));

    for (QString setCode : jsonRsp.keys()){
        QString setCodeVersion = QString("%1_%2").arg(setCode).arg(jsonRsp[setCode].toString());
        loadSet(setCodeVersion);
    }
}

void MtgCards::loadSet(QString setCodeVersion)
{
    QFile setFile(setsDir + QDir::separator() + setCodeVersion + ".json");
    if (QFileInfo(setFile).exists()) {
        loadSetFromFile(setCodeVersion + ".json");
    } else {
        downloadSet(setCodeVersion);
    }
}

void MtgCards::downloadSet(QString setCodeVersion)
{
    QString setCode = setCodeVersion.left(setCodeVersion.indexOf("_"));
    QString version = setCodeVersion.right(setCodeVersion.indexOf("_") - 1);
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("set", setCode);
    urlQuery.addQueryItem("version", version);
    QUrl url(QString("%1/sets").arg(URLs::API()));
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    QNetworkReply *reply = networkManager.get(request);
    connect(reply, &QNetworkReply::finished,
            this, &MtgCards::downloadSetOnFinish);
    LOGD(QString("Downloading %1 cards").arg(setCode));
}

void MtgCards::downloadSetOnFinish()
{
    QNetworkReply *reply = static_cast<QNetworkReply*>(sender());
    QByteArray jsonData = reply->readAll();

    QString setUrl = reply->url().toString();
    if (reply->error() == QNetworkReply::ContentNotFoundError) {
        LOGW(QString("Error while downloading mtg card json: %1").arg(setUrl));
        return;
    }

    QJsonArray jsonSet = Transformations::stringToJsonArray(jsonData);
    if (jsonSet.empty()) {
        return;
    }
    QString query = reply->request().url().query();
    QString setCode = QUrlQuery(query).queryItemValue("set");
    QString version = QUrlQuery(query).queryItemValue("version");
    LOGD(QString("Downloaded %1 bytes from %2 json").arg(jsonData.size()).arg(setCode));

    QString setCodeVersion = QString("%1_%2").arg(setCode).arg(version);
    QFile setFile(setsDir + QDir::separator() + setCodeVersion + ".json");
    setFile.open(QIODevice::WriteOnly);
    setFile.write(jsonData);
    setFile.close();

    loadSetFromFile(setCodeVersion + ".json");
}

void MtgCards::loadSetFromFile(QString setFileName) {
    QString setCode = setFileName.left(setFileName.indexOf("v") - 1);
    LOGD(QString("Loading %1").arg(setCode));

    QFile setFile(setsDir + QDir::separator() + setFileName);
    if (!QFileInfo(setFile).exists()) {
        LOGW(QString("%1 not found.").arg(setFileName));
        return;
    }

    bool opened = setFile.open(QIODevice::ReadOnly | QIODevice::Text);
    if (!opened) {
        LOGW(QString("Error while opening %1.").arg(setFileName));
        return;
    }

    QByteArray jsonData = setFile.readAll();
    QJsonArray jsonCards = Transformations::stringToJsonArray(jsonData);

    for (QJsonValueRef jsonCardRef: jsonCards) {
        QJsonObject jsonCard = jsonCardRef.toObject();
        Card* card = jsonObject2Card(jsonCard, setCode);
        cards[card->mtgaId] = card;
        QString layout = jsonCard["layout"].toString();
        if (layout == "split" && card->number.endsWith("b")) {
            QString downSideNumber = card->number;
            QString upSideNumber = downSideNumber.replace("b", "a");
            int upSideMtgaId = 0;
            for (int mtgaid : cards.keys()) {
                if (cards[mtgaid]->setCode == setCode &&
                        cards[mtgaid]->number == upSideNumber) {
                    upSideMtgaId = mtgaid;
                    break;
                }
            }
            Card* cardUp = cards[upSideMtgaId];
            Card* cardSplit = createSplitCard(cardUp, card);
            cards[cardSplit->mtgaId] = cardSplit;
        }
    }

    LOGI(QString("%1 set loaded with %2 cards").arg(setCode).arg(jsonCards.count()));
}

Card* MtgCards::jsonObject2Card(QJsonObject jsonCard, QString setCode)
{
    int mtgaId = jsonCard["mtgaid"].toInt();
    int multiverseId = jsonCard["multiverseid"].toInt();
    QString number = jsonCard["number"].toString();
    QString name = jsonCard["name"].toString();
    QString layout = jsonCard["layout"].toString();
    QString imageUrl = jsonCard["imageUrl"].toString();
    QString type = jsonCard["type"].toString();
    QJsonArray jsonTypes = jsonCard["types"].toArray();
    bool isArtifact = false;
    bool isLand = false;
    for (QJsonValueRef typeRef : jsonTypes) {
        if (typeRef.toString().contains("Artifact")) {
            isArtifact = true;
        }
        if (typeRef.toString().contains("Land")) {
            isLand = true;
        }
    }
    // Mana color
    QString rawManaCost = jsonCard["manaCost"].toString();
    QRegularExpression reManaSymbol("(?<=\\{)[\\w,\\/]+(?=\\})");
    QRegularExpressionMatchIterator iterator = reManaSymbol.globalMatch(rawManaCost);
    QString manaCost;
    QRegExp reDigits("\\d+");  // a digit (\d), one or more times (*)
    while (iterator.hasNext()) {
        QString manaSymbol = iterator.next().captured(0).toLower();
        if (manaSymbol.size() == 1 || reDigits.exactMatch(manaSymbol)) {
            manaCost += manaSymbol;
        } else if (manaSymbol.contains('/')) {
            for (QString mana : manaSymbol.split('/')) {
                if (mana.size() > 1) {
                    LOGD(QString(tr("Unkown mana symbol: %1")).arg(mana))
                    break;
                }
                if (!manaCost.contains(mana)) {
                    manaCost += mana;
                }
            }
        } else {
            LOGD(QString(tr("Unkown mana symbol: %1")).arg(manaSymbol))
        }
    }
    // Color identity
    QList<QChar> manaColorIdentity = getBoderColorUsingManaCost(manaCost, isArtifact);
    QList<QChar> borderColorIdentity = manaColorIdentity;
    if (isArtifact) {
        borderColorIdentity.clear();
        borderColorIdentity << 'a';
    }
    if (isLand) {
        borderColorIdentity = getLandBorderColorUsingColorIdentity(jsonCard);
    }
    return new Card(mtgaId, multiverseId, setCode, number, name, type, layout, manaCost,
                    borderColorIdentity, manaColorIdentity, imageUrl, isLand, isArtifact);
}

QList<QChar> MtgCards::getBoderColorUsingManaCost(QString manaCost, bool isArtifact)
{
    QList<QChar> manaSymbols;
    for (QChar manaSymbol : manaCost) {
        if (manaSymbol.isLetter() && manaSymbol != 'x'
                && manaSymbol != 'y' && manaSymbol != 'z'){
            if (!manaSymbols.contains(manaSymbol)){
                manaSymbols << manaSymbol;
            }
        }
    }
    if (manaSymbols.size() >= 3) {
        manaSymbols.clear();
        manaSymbols << QChar('m');
    }
    if (manaSymbols.isEmpty()) {
        if (isArtifact) {
            manaSymbols << 'a';
        } else {
            manaSymbols << 'c';
        }
    }
    return manaSymbols;
}

QList<QChar> MtgCards::getLandBorderColorUsingColorIdentity(QJsonObject jsonCard)
{
    QList<QChar> borderColorIdentity;
    QJsonArray jsonColorIdentity = jsonCard["colorIdentity"].toArray();
    QString text = jsonCard["text"].toString();
    for (QJsonValueRef colorIdentityRef : jsonColorIdentity) {
        borderColorIdentity << colorIdentityRef.toString().toLower().at(0);
    }
    if (borderColorIdentity.size() >= 3) {
        borderColorIdentity.clear();
        borderColorIdentity << QChar('m');
    }
    if (borderColorIdentity.isEmpty()) {
        borderColorIdentity << QChar(text.contains("mana of any color") ? 'm' : 'c');
    }
    return borderColorIdentity;
}

Card* MtgCards::createSplitCard(Card* upSide, Card* downSide)
{
    QString name = QString("%1 // %2").arg(upSide->name).arg(downSide->name);
    QList<QChar> borderColorIdentity = upSide->borderColorIdentity;
    for (QChar c : downSide->borderColorIdentity){
        if (!borderColorIdentity.contains(c)) {
            borderColorIdentity.append(c);
        }
    }
    QList<QChar> manaColorIdentity = upSide->manaColorIdentity;
    for (QChar c : downSide->manaColorIdentity){
        if (!manaColorIdentity.contains(c)) {
            manaColorIdentity.append(c);
        }
    }
    QString leftSideNumber = upSide->number;
    QString number = leftSideNumber.replace("a", "");
    int mtgaIdSidesDiff = downSide->mtgaId - upSide->mtgaId;
    int mtgaId = upSide->mtgaId - mtgaIdSidesDiff;
    return new Card(mtgaId, upSide->multiverseId, upSide->setCode, number,
                    name, upSide->type, upSide->layout, upSide->manaCost,
                    borderColorIdentity, manaColorIdentity, upSide->imageUrl,
                    upSide->isLand, upSide->isArtifact);
}

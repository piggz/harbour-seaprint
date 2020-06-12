#include "ippprinter.h"
#include <seaprint_version.h>
#include "mimer.h"
#include "papersizes.h"

IppPrinter::IppPrinter()
{
    _nam = new QNetworkAccessManager(this);
    _print_nam = new QNetworkAccessManager(this);
    _jobs_nam = new QNetworkAccessManager(this);
    _job_cancel_nam = new QNetworkAccessManager(this);

    connect(_nam, &QNetworkAccessManager::finished, this, &IppPrinter::getPrinterAttributesFinished);
    connect(_nam, &QNetworkAccessManager::sslErrors, this, &IppPrinter::ignoreKnownSslErrors);

    connect(_print_nam, &QNetworkAccessManager::finished, this, &IppPrinter::printRequestFinished);
    connect(_print_nam, &QNetworkAccessManager::sslErrors, this, &IppPrinter::ignoreKnownSslErrors);

    connect(_jobs_nam, &QNetworkAccessManager::finished,this, &IppPrinter::getJobsRequestFinished);
    connect(_jobs_nam, &QNetworkAccessManager::sslErrors, this, &IppPrinter::ignoreKnownSslErrors);

    connect(_job_cancel_nam, &QNetworkAccessManager::finished,this, &IppPrinter::cancelJobFinished);
    connect(_job_cancel_nam, &QNetworkAccessManager::sslErrors, this, &IppPrinter::ignoreKnownSslErrors);

    QObject::connect(this, &IppPrinter::urlChanged, this, &IppPrinter::onUrlChanged);
    qRegisterMetaType<QTemporaryFile*>("QTemporaryFile*");

    _worker = new ConvertWorker;
    _worker->moveToThread(&_workerThread);

    connect(&_workerThread, &QThread::finished, _worker, &QObject::deleteLater);

    connect(this, &IppPrinter::doConvertPdf, _worker, &ConvertWorker::convertPdf);
    connect(this, &IppPrinter::doConvertImage, _worker, &ConvertWorker::convertImage);
    connect(_worker, &ConvertWorker::done, this, &IppPrinter::convertDone);
    connect(_worker, &ConvertWorker::failed, this, &IppPrinter::convertFailed);

    _workerThread.start();
}

IppPrinter::~IppPrinter() {
    delete _nam;
    delete _print_nam;
    delete _jobs_nam;
    delete _job_cancel_nam;
}

QJsonObject IppPrinter::opAttrs() {
    QJsonObject o
    {
        {"attributes-charset",          QJsonObject {{"tag", IppMsg::Charset},             {"value", "utf-8"}}},
        {"attributes-natural-language", QJsonObject {{"tag", IppMsg::NaturalLanguage},     {"value", "en-us"}}},
        {"printer-uri",                 QJsonObject {{"tag", IppMsg::Uri},                 {"value", _url.toString()}}},
        {"requesting-user-name",        QJsonObject {{"tag", IppMsg::NameWithoutLanguage}, {"value", "nemo"}}},
    };
    return o;
}

void IppPrinter::setUrl(QString url_s)
{
    QUrl url = QUrl(url_s);

    qDebug() << url.scheme();

    if(url.scheme() != "ipp" /* or ipps */)
    {
        //if https -> ipps, else:
        if(url.scheme() == "")
        {
            url = QUrl("ipp://"+url_s); // Why isn't setScheme working?
        }
        else if (url.scheme() == "http") {
            url.setScheme("ipp");
        }
        else {
            url = QUrl();
        }
    }

    qDebug() << url_s << url;

    if(url != _url)
    {
        _url = url;
        emit urlChanged();
    }
}

void IppPrinter::onUrlChanged()
{
    refresh();
}

void IppPrinter::refresh() {
    _attrs = QJsonObject();
    emit attrsChanged();

    _additionalDocumentFormats = QStringList();
    emit additionalDocumentFormatsChanged();


    QNetworkRequest request;

    request.setUrl(httpUrl());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/ipp");
    request.setHeader(QNetworkRequest::UserAgentHeader, "SeaPrint " SEAPRINT_VERSION);

    QJsonObject o = opAttrs();
    IppMsg msg = IppMsg(o);
    _nam->post(request, msg.encode(IppMsg::GetPrinterAttrs));

}

void IppPrinter::getPrinterAttributesFinished(QNetworkReply *reply)
{
    qDebug() << reply->error() << reply->errorString() << reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toString();
    _attrs = QJsonObject();
    if(reply->error()  == QNetworkReply::NoError)
    {
        try {
            IppMsg resp(reply);
            qDebug() << resp.getStatus() << resp.getOpAttrs() << resp.getPrinterAttrs();
            _attrs = resp.getPrinterAttrs();
            if(resp.getOpAttrs().keys().contains("status-message"))
            { // Sometimes there are no response attributes at all,
              // maybe status-message from the operation attributes is somewhat useful
                _attrs["status-message"] = resp.getOpAttrs()["status-message"];
            }
        }
        catch(std::exception e)
        {
            qDebug() << e.what();
        }
    }

    if(_attrs.contains("printer-device-id"))
    {
        QJsonArray supportedMimeTypes = _attrs["document-format-supported"].toObject()["value"].toArray();
        QStringList printerDeviceId = _attrs["printer-device-id"].toObject()["value"].toString().split(";");
        for (QStringList::iterator it = printerDeviceId.begin(); it != printerDeviceId.end(); it++)
        {
            QStringList kv = it->split(":");
            if(kv.length()==2 && kv[0]=="CMD")
            {
                QStringList cmds = kv[1].split(",");
                if(cmds.contains("PDF") && !supportedMimeTypes.contains("application/pdf"))
                {
                    _additionalDocumentFormats.append("application/pdf");
                }
                if(cmds.contains("POSTSCRIPT") && !supportedMimeTypes.contains("application/postscript"))
                {
                    _additionalDocumentFormats.append("application/postscript");
                }
            }
        }
        qDebug() << "additionalDocumentFormats" << _additionalDocumentFormats;
        emit additionalDocumentFormatsChanged();
    }

    emit attrsChanged();
}

void IppPrinter::printRequestFinished(QNetworkReply *reply)
{
    _jobAttrs = QJsonObject();
    bool status = false;
    qDebug() << "Finished:" << reply->readAll();
    if(reply->error()  == QNetworkReply::NoError)
    {
        try {
            IppMsg resp(reply);
            qDebug() << resp.getStatus() << resp.getOpAttrs() << resp.getJobAttrs();
            _jobAttrs = resp.getJobAttrs()[0].toObject();
            status = resp.getStatus() <= 0xff;
        }
        catch(std::exception e)
        {
            qDebug() << e.what();
        }
    }
    else {
        _jobAttrs.insert("job-state-message", QJsonObject {{"tag", IppMsg::TextWithoutLanguage}, {"value", "Network error"}});
    }
    emit jobAttrsChanged();
    emit jobFinished(status);
}

void IppPrinter::getJobsRequestFinished(QNetworkReply *reply)
{
    if(reply->error()  == QNetworkReply::NoError)
    {
        try {
            IppMsg resp(reply);
            qDebug() << resp.getStatus() << resp.getOpAttrs() << resp.getJobAttrs();
            _jobs = resp.getJobAttrs();
            emit jobsChanged();
        }
        catch(std::exception e)
        {
            qDebug() << e.what();
        }
    }
}


void IppPrinter::cancelJobFinished(QNetworkReply *reply)
{
    bool status = false;
    if(reply->error()  == QNetworkReply::NoError)
    {
        try {
            IppMsg resp(reply);
            qDebug() << resp.getStatus() << resp.getOpAttrs() << resp.getJobAttrs();
            status = resp.getStatus() <= 0xff;
        }
        catch(std::exception e)
        {
            qDebug() << e.what();
        }
    }
    emit cancelStatus(status);
    getJobs();
}



void IppPrinter::ignoreKnownSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
    QList<QSslError> IgnoredSslErrors = {QSslError::NoError,
                                         QSslError::SelfSignedCertificate,
                                         QSslError::HostNameMismatch,
                                         QSslError::UnableToGetLocalIssuerCertificate,
                                         QSslError::UnableToVerifyFirstCertificate
                                         };

    qDebug() << errors;
    for (QList<QSslError>::const_iterator it = errors.constBegin(); it != errors.constEnd(); it++) {
        if(!IgnoredSslErrors.contains(it->error())) {
            qDebug() << "Bad error: " << int(it->error()) <<  it->error();
            return;
        }
    }
    // For whatever reason, it doesn't work to pass IgnoredSslErrors here
    reply->ignoreSslErrors(errors);
}

void IppPrinter::convertDone(QNetworkRequest request, QTemporaryFile* data)
{
    connect(_print_nam, SIGNAL(finished(QNetworkReply*)), data, SLOT(deleteLater()));
    data->open();

    setBusyMessage("Transferring");

    QNetworkReply* reply = _print_nam->post(request, data);

    connect(reply, &QNetworkReply::uploadProgress, this, &IppPrinter::setProgress);

}

void IppPrinter::convertFailed(QString message)
{
    _jobAttrs = QJsonObject();
    _jobAttrs.insert("job-state-message", QJsonObject {{"tag", IppMsg::TextWithoutLanguage}, {"value", message}});
    emit jobAttrsChanged();
    emit jobFinished(false);
}

QString firstMatch(QJsonArray supported, QStringList wanted)
{
    for(QStringList::iterator it = wanted.begin(); it != wanted.end(); it++)
    {
        if(supported.contains(*it))
        {
            return *it;
        }
    }
    return "";
}

QString targetFormatIfAuto(QString documentFormat, QString mimeType, QJsonArray supportedMimeTypes, bool forceRaster)
{
    if(forceRaster)
    {
        return firstMatch(supportedMimeTypes, {"image/pwg-raster", "image/urf"});
    }
    else if(documentFormat == "application/octet-stream")
    {
        if(mimeType == "application/pdf")
        {
            return firstMatch(supportedMimeTypes, {"application/pdf", "application/postscript", "image/pwg-raster", "image/urf" });
        }
        else if (mimeType.contains("image"))
        {
            return firstMatch(supportedMimeTypes, {"image/png", "image/gif", "image/jpeg", "image/pwg-raster", "image/urf"});
        }
        return documentFormat;
    }
    return documentFormat;
}

void IppPrinter::print(QJsonObject attrs, QString filename,
                       bool alwaysConvert, bool forceIncluDeDocumentFormat, bool removeRedundantConvertAttrs)
{
    qDebug() << "printing" << filename << attrs
             << alwaysConvert << forceIncluDeDocumentFormat << removeRedundantConvertAttrs;

    _progress = "";
    emit progressChanged();

    QFile file(filename);
    bool file_ok = file.open(QIODevice::ReadOnly);
    if(!file_ok)
    {
        emit convertFailed(tr("Failed to open file"));
        return;
    }

    Mimer* mimer = Mimer::instance();
    QString mimeType = mimer->get_type(filename);


    QJsonArray supportedMimeTypes = _attrs["document-format-supported"].toObject()["value"].toArray();
    for(QStringList::iterator it = _additionalDocumentFormats.begin(); it != _additionalDocumentFormats.end(); it++)
    {
        supportedMimeTypes.append(*it);
    }

    qDebug() << supportedMimeTypes << supportedMimeTypes.contains(mimeType);

    QFileInfo fileinfo(file);

    QJsonObject o = opAttrs();
    o.insert("job-name", QJsonObject {{"tag", IppMsg::NameWithoutLanguage}, {"value", fileinfo.fileName()}});

    QJsonArray jobCreationAttributes = _attrs["job-creation-attributes-supported"].toObject()["value"].toArray();

    QString documentFormat = getAttrOrDefault(attrs, "document-format").toString();
    qDebug() << "target format:" << documentFormat << "alwaysConvert:" << alwaysConvert;

    documentFormat = targetFormatIfAuto(documentFormat, mimeType, supportedMimeTypes, alwaysConvert);
    qDebug() << "adjusted target format:" << documentFormat;

    if(documentFormat == "" || documentFormat == "application/octet-string")
    {
        emit convertFailed(tr("Unknown document format"));
        return;
    }

    if(!jobCreationAttributes.contains("document-format") && !forceIncluDeDocumentFormat)
    { // Only include if printer supports it
        attrs.remove("document-format");
    }

    qDebug() << "Printing job" << o << attrs;

    QNetworkRequest request;

    request.setUrl(httpUrl());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/ipp");
    request.setHeader(QNetworkRequest::UserAgentHeader, "SeaPrint " SEAPRINT_VERSION);

    QJsonValue PrinterResolutionRef = getAttrOrDefault(attrs, "printer-resolution");
    quint32 HwResX = PrinterResolutionRef.toObject()["x"].toInt();
    quint32 HwResY = PrinterResolutionRef.toObject()["y"].toInt();

    if(documentFormat == "image/urf")
    { // Ensure symmetric resolution for URF
        if(HwResX < HwResY)
        {
            HwResY = HwResX;
        }
        else
        {
            HwResX = HwResY;
        }
        QJsonObject tmpObj;
        tmpObj["units"] = PrinterResolutionRef.toObject()["units"];
        tmpObj["x"] = (int)HwResX;
        tmpObj["y"] = (int)HwResY;
        attrs["printer-resolution"] = tmpObj;
    }

    quint32 Quality = getAttrOrDefault(attrs, "print-quality").toInt();

    QString PrintColorMode = getAttrOrDefault(attrs, "print-color-mode").toString();
    quint32 Colors = PrintColorMode.contains("color") ? 3 : PrintColorMode.contains("monochrome") ? 1 : 0;

    QString PaperSize = getAttrOrDefault(attrs, "media").toString();
    if(!PaperSizes.contains(PaperSize))
    {
        emit convertFailed(tr("Unsupported print media"));
        return;
    }

    QString Sides = getAttrOrDefault(attrs, "sides").toString();
    if(removeRedundantConvertAttrs && (documentFormat=="image/pwg-raster" ||
                                              documentFormat=="image/urf"))
    {
        attrs.remove("sides");
        attrs.remove("print-color-mode");
    }
    if(removeRedundantConvertAttrs && documentFormat == "application/postscript")
    {
        attrs.remove("sides");
    }

    //PGZ TEMP
    QJsonObject mediacol
    {
        QJsonObject {{"tag", IppMsg::BeginCollection}, {"value",
                    QJsonObject{
                            {"media-size", QJsonObject{{"tag", IppMsg::BeginCollection}, {"value",
                                QJsonObject {
                                    {"x-dimension", QJsonObject{{"tag", IppMsg::Integer}, {"value", 12700}}},
                                    {"y-dimension", QJsonObject{{"tag", IppMsg::Integer}, {"value", 17780}}}
                                }
                                }
                            }
                          }
                    }
                }
            }

    };
    attrs.insert("media-col", mediacol);
    //END

    qDebug() << "Final job attributes:" << attrs;

    IppMsg job = IppMsg(o, attrs);
    QByteArray contents = job.encode(IppMsg::PrintJob);
                                       // Always convert images to get resizing
    //if((mimeType == documentFormat) && !mimeType.contains("image"))
    //{
        qDebug() << "Contents:" << contents;

        QByteArray filedata = file.readAll();
        contents = contents.append(filedata);
        file.close();

        setBusyMessage("Transferring");
        QNetworkReply* reply = _print_nam->post(request, contents);
        connect(reply, &QNetworkReply::uploadProgress, this, &IppPrinter::setProgress);
        return;
    //}
    //else
    {
        file.close();

        QTemporaryFile* tempfile = new QTemporaryFile();
        tempfile->open();
        tempfile->write(contents);
        qDebug() << tempfile->fileName();
        tempfile->close();

        setBusyMessage("Converting");

        if(mimeType == "application/pdf")
        {
            bool TwoSided = false;
            bool Tumble = false;
            if(Sides=="two-sided-long-edge")
            {
                TwoSided = true;
            }
            else if(Sides=="two-sided-short-edge")
            {
                TwoSided = true;
                Tumble = true;
            }

            emit doConvertPdf(request, filename, tempfile, documentFormat, Colors, Quality,
                              PaperSize, HwResX, HwResY, TwoSided, Tumble);
        }
        else if (mimeType.contains("image"))
        {
            emit doConvertImage(request, filename, tempfile, documentFormat, Colors, Quality,
                                PaperSize, HwResX, HwResY);
        }
        else
        {
            emit convertFailed(tr("Cannot convert this file format"));
            return;
        }
    }

    return;
}

bool IppPrinter::getJobs() {

    qDebug() << "getting jobs";

    QJsonObject o = opAttrs();
    o.insert("requested-attributes", QJsonObject {{"tag", IppMsg::Keyword}, {"value", "all"}});

    IppMsg job = IppMsg(o, QJsonObject());

    QNetworkRequest request;

    QByteArray contents = job.encode(IppMsg::GetJobs);

    request.setUrl(httpUrl());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/ipp");
    request.setHeader(QNetworkRequest::UserAgentHeader, "SeaPrint " SEAPRINT_VERSION);

    _jobs_nam->post(request, contents);

    return true;
}

bool IppPrinter::cancelJob(qint32 jobId) {

    qDebug() << "cancelling jobs";

    QJsonObject o = opAttrs();
    o.insert("job-id", QJsonObject {{"tag", IppMsg::Integer}, {"value", jobId}});

    IppMsg job = IppMsg(o, QJsonObject());

    QNetworkRequest request;

    QByteArray contents = job.encode(IppMsg::CancelJob);

    request.setUrl(httpUrl());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/ipp");
    request.setHeader(QNetworkRequest::UserAgentHeader, "SeaPrint " SEAPRINT_VERSION);

    _job_cancel_nam->post(request, contents);

    return true;
}

QUrl IppPrinter::httpUrl() {
    QUrl url = _url;
    url.setScheme("http");
    if(url.port() == -1) {
        url.setPort(631);
    }
    return url;
}

void IppPrinter::setBusyMessage(QString msg)
{
    _busyMessage = msg;
    emit busyMessageChanged();
}

void IppPrinter::setProgress(qint64 sent, qint64 total)
{
    if(total == 0)
        return;

    _progress = QString::number(100*sent/total);
    _progress += "%";
    emit progressChanged();
}

QJsonValue IppPrinter::getAttrOrDefault(QJsonObject jobAttrs, QString name)
{
    if(jobAttrs.contains(name))
    {
        return jobAttrs[name].toObject()["value"];
    }
    else {
        return _attrs[name+"-default"].toObject()["value"];
    }
}

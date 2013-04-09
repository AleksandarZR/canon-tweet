#include <QNetworkAccessManager>
#include <QtWidgets/QApplication>
#include <QStringList>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include "phototweet.h"
#include "oauthtwitter.h"
#include "qtweetstatusupdate.h"
#include "qtweetstatus.h"
#include "qtweetconfiguration.h"
#include <QJsonDocument>
#include <QJsonObject>
#include "twitpicUpload.h"
#include "twitpicUploadStatus.h"
#include "yfrogUpload.h"
#include "yfrogUploadStatus.h"

PhotoTweet::PhotoTweet() :
m_quit(true),
m_idle(true)
{
	// Setup the camera system.
	QString imageDir = QCoreApplication::applicationDirPath() + "/images";
	m_camera = new Camera(imageDir);

	connect(m_camera, SIGNAL(OnTakePictureSuccess(QString)), SLOT(takePictureSuccess(QString)));
	connect(m_camera, SIGNAL(OnTakePictureError(Camera::ErrorType, int)), SLOT(takePictureError(Camera::ErrorType, int)));

	int error = m_camera->Startup();
	if (error != EDS_ERR_OK)
	{
		qCritical("Couldn't start the camera system!");
		qCritical("%s", Camera::GetErrorMessage(Camera::ErrorType_Normal, error).toLatin1().constData());
	}

    m_oauthTwitter = new OAuthTwitter(this);
    m_oauthTwitter->setNetworkAccessManager(new QNetworkAccessManager(this));

	// Setup tweet config access.
    m_tweetConfig = new QTweetConfiguration(m_oauthTwitter, this);
    connect(m_tweetConfig, SIGNAL(configuration(QJsonDocument)), SLOT(getConfigurationFinished(QJsonDocument)));
    connect(m_tweetConfig, SIGNAL(error(QTweetNetBase::ErrorCode, QString)), SLOT(postStatusError(QTweetNetBase::ErrorCode, QString)));

	// Setup status update access.
	m_statusUpdate = new QTweetStatusUpdate(m_oauthTwitter, this);
    connect(m_statusUpdate, SIGNAL(postedStatus(QTweetStatus)), SLOT(postStatusFinished(QTweetStatus)));
    connect(m_statusUpdate, SIGNAL(error(QTweetNetBase::ErrorCode, QString)), SLOT(postStatusError(QTweetNetBase::ErrorCode, QString)));

	// Setup twitpic uploads.
	m_twitpic = new TwitpicUpload(m_twitpicApiKey, m_oauthTwitter, this);
	connect(m_twitpic, SIGNAL(jsonParseError(QByteArray)), SLOT(twitpicJsonParseError(QByteArray)));
    connect(m_twitpic, SIGNAL(error(QTweetNetBase::ErrorCode, QString)), SLOT(twitpicError(QTweetNetBase::ErrorCode, QString)));
    connect(m_twitpic, SIGNAL(finished(TwitpicUploadStatus)), SLOT(twitpicFinished(TwitpicUploadStatus)));

	// Setup yfrog uploads.
	m_yfrog = new YfrogUpload(m_yfrogApiKey, m_oauthTwitter, this);
    connect(m_yfrog, SIGNAL(error(QTweetNetBase::ErrorCode, YfrogUploadStatus)), SLOT(yfrogError(QTweetNetBase::ErrorCode, YfrogUploadStatus)));
    connect(m_yfrog, SIGNAL(finished(YfrogUploadStatus)), SLOT(yfrogFinished(YfrogUploadStatus)));
}

PhotoTweet::~PhotoTweet()
{
	m_camera->Shutdown();
}

bool PhotoTweet::loadConfig()
{
    QFile configFile("phototweet.cfg");
    if (!configFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qWarning("Couldn't load config file 'phototweet.cfg'");
        return false;
    }

    QTextStream reader(&configFile);
    while (!reader.atEnd())
    {
        QString line = reader.readLine().trimmed();
        if (line.length() > 0 && line.at(0) == '[' && line.endsWith(']'))
        {
            QString key = line.mid(1, line.length() - 2);
            QString val = reader.readLine().trimmed();
            if (val.length() > 0)
            {
                if (!key.compare("consumer_key"))
                {
                    m_oauthTwitter->setConsumerKey(val.toLatin1());
                }
                else if (!key.compare("consumer_secret"))
                {
                    m_oauthTwitter->setConsumerSecret(val.toLatin1());
                }
                else if (!key.compare("oauth_token"))
                {
                    m_oauthTwitter->setOAuthToken(val.toLatin1());
                }
                else if (!key.compare("oauth_token_secret"))
                {
                    m_oauthTwitter->setOAuthTokenSecret(val.toLatin1());
                }
				else if (!key.compare("twitpic_api_key"))
				{
					m_twitpicApiKey = val;
				}
				else if(!key.compare("yfrog_api_key"))
				{
					m_yfrogApiKey = val;
				}
            }
        }
    }

    return true;
}

void PhotoTweet::showUsage()
{
    printf("photoTweet.exe [args]\n\n");
    printf("where args are:\n\n");
    printf("-image <image path>         upload specified image\n");
    printf("-message <message text>     tweet specified status text\n");
    printf("-getconfig                  return media configuration settings\n");
	printf("-continuous <time>          take photo and tweet every <time> seconds\n");
	printf("-takePhotoAndTweet          take a photo and tweet it\n");
    printf("\n");
    printf("Note: a message must always be specified if tweeting.  Use quotes\n");
    printf("around the message text to specify multiple words.\n");
}

void PhotoTweet::takePhotoAndTweet()
{
	if (m_idle)
	{
		m_idle = false;
		qDebug("Taking a photo..");
		m_camera->TakePicture();
	}
	else
	{
		qDebug("Photo in progress..");
	}
}

void PhotoTweet::takePictureSuccess(const QString& filePath)
{
	uploadAndTweet(QString(), filePath);
}

void PhotoTweet::takePictureError(Camera::ErrorType errorType, int error)
{
	qCritical("Couldn't take a picture!");
	qCritical("%s", Camera::GetErrorMessage(errorType, error).toLatin1().constData());
	m_idle = true;
}

void PhotoTweet::main()
{
    QStringList& args = QApplication::arguments();
    bool error = false, continuous = false;
	float shotTime = 2.0f;

    QString message, imagePath;

    for (int i = 0; i < args.count(); i++)
    {
        if (!args.at(i).compare("-message"))
        {
            if (i + 1 < args.count())
            {
                message = args.at(i + 1);
            }
            else
            {
                printf("Missing argument to -message!\n");
                error = true;
                break;
            }
        }
        else if (!args.at(i).compare("-image"))
        {
            if (i + 1 < args.count())
            {
                imagePath = args.at(i + 1);
                if (!QFile::exists(imagePath))
                {
                    printf("Couldn't find file %s!\n", imagePath.toLatin1().constData());
                    error = true;
                }
            }
            else
            {
                printf("Missing argument to -image!\n");
                error = true;
                break;
            }
        }
        else if (!args.at(i).compare("-getconfig"))
        {
            m_tweetConfig->get();
            return;
        }
		else if (!args.at(i).compare("-continuous"))
		{
			if (i + 1 < args.count())
			{
				shotTime = args.at(i + 1).toFloat();
				continuous = true;
				m_quit = false;
			}
			else
			{
				printf("Missing argument to -continuous!\n");
				error = true;
				break;
			}
		}
		else if (!args.at(i).compare("-takePhotoAndTweet"))
		{
			return takePhotoAndTweet();
		}
    }

	if (continuous)
	{
		QTimer* timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), this, SLOT(takePhotoAndTweet()));
		timer->start(shotTime * 1000);
		return;
	}

    if (message.length() == 0)
    {
        showUsage();
        return doQuit();
    }

    if (error)
    {
        return doQuit();
    }

    uploadAndTweet(message, imagePath);
}

void PhotoTweet::uploadAndTweet(const QString& message, const QString& imagePath)
{
    m_message = message;

	if (m_message.length() > 0)
	{
		qDebug("Tweeting message: '%s'", m_message.toLatin1().constData());
	}

    if (imagePath.length() > 0)
    {
        qDebug("Uploading image: '%s'", imagePath.toLatin1().constData());
		postMessageWithImageYfrog(imagePath);
    }
    else
    {
        postMessage();
    }
}

void PhotoTweet::doQuit()
{
    emit quit();
}

void PhotoTweet::printObject(const QVariant& object)
{
    if (object.type() == QVariant::Map)
    {
        QVariantMap m = object.toMap();
        QList<QString> keys = m.keys();
        for (int i = 0; i < keys.count(); i++)
        {
            QVariant value = m[keys[i]];
            QString key = keys[i];
            printf("%s=", key.toLatin1().constData());
            if (value.type() == QVariant::String || value.type() == QVariant::Int || value.type() == QVariant::Double)
            {
                printf("%s", value.toString().toLatin1().constData());
            }
            else
            {
                printf("(%s)", value.typeName());
            }
            if (i < keys.count() - 1)
            {
                printf(", ");
            }
        }
    }
}

void PhotoTweet::getConfigurationFinished(const QJsonDocument& json)
{
    QJsonObject response = json.object();
    QStringList& keys = response.keys();
    for (int i = 0; i < keys.count(); i++)
    {
        QString key = keys[i];
        QJsonValue value = response[key];
        printf("%s=", key.toLatin1().constData());

        if (value.isArray())
        {
            printf("<array>\n");
        }
        else if (value.isObject())
        {
            printf("\n");
            QVariant v = value.toVariant();
            QVariantMap m = v.toMap();
            QList<QString> k = m.keys();
            for (int i = 0; i < k.count(); i++)
            {
                printf("   %s={ ", k[i].toLatin1().constData());
                printObject(m[k[i]]);
                printf(" }\n");
            }
        }
        else
        {
            QVariant v = value.toVariant();
            QString s = v.toString();
            printf("%s\n", s.toLatin1().constData());
        }
    }

	if (m_quit)
	{
		return doQuit();
	}
}

void PhotoTweet::postMessage()
{
	m_statusUpdate->post(m_message);
}

void PhotoTweet::postMessageWithImageTwitpic(const QString& imagePath)
{
	if (QFile::exists(imagePath))
	{
		m_twitpic->upload(imagePath);
	}
	else
	{
		qWarning("Couldn't find file %s", imagePath.toLatin1().constData());
	}
}

void PhotoTweet::postMessageWithImageYfrog(const QString& imagePath)
{
	if (QFile::exists(imagePath))
	{
		m_yfrog->upload(imagePath);
	}
	else
	{
		qWarning("Couldn't find file %s", imagePath.toLatin1().constData());
	}
}

void PhotoTweet::postStatusFinished(const QTweetStatus &status)
{
	qDebug("Posted status with id %llu", status.id());

	if (m_quit)
	{
		doQuit();
	}

	m_idle = true;
}

void PhotoTweet::postStatusError(QTweetNetBase::ErrorCode, QString errorMsg)
{
    if (errorMsg.length() > 0)
    {
        qWarning("Error posting message: %s", errorMsg.toLatin1().constData());
    }

	if (m_quit)
	{
		doQuit();
	}

	m_idle = true;
}

void PhotoTweet::twitpicError(QTweetNetBase::ErrorCode, QString errorMsg)
{
	qWarning("Error posting image to twitpic: %s", errorMsg.toLatin1().constData());

	if (m_quit)
	{
		doQuit();
	}
}

void PhotoTweet::twitpicJsonParseError(const QByteArray& json)
{
	qWarning("Error parsing json result while posting image to twitpic");
	qWarning("json: %s", json.constData());

	if (m_quit)
	{
		doQuit();
	}
}

void PhotoTweet::twitpicFinished(const TwitpicUploadStatus& status)
{
	qDebug("Posted image to twitpic!");
	qDebug("Url is %s", status.getImageUrl().toLatin1().constData());
	if (m_message.length() > 0)
	{
		m_message += " ";
	}
	m_message += status.getImageUrl();
	qDebug("Posting link to twitter..");
	postMessage();
}

void PhotoTweet::yfrogError(QTweetNetBase::ErrorCode code, const YfrogUploadStatus& status)
{
	qWarning("Error posting image to yfrog: %s (%i)", status.getHttpStatusString(), code);

	m_idle = true;

	if (m_quit)
	{
		doQuit();
	}
}

void PhotoTweet::yfrogFinished(const YfrogUploadStatus& status)
{
	if (status.getStatus() != YfrogUploadStatus::Ok)
	{
		qWarning("Error posting image to yfrog: %s", status.getStatusString().toLatin1().constData());

		if (m_quit)
		{
			doQuit();
		}

		m_idle = true;
	}
	else
	{
		qDebug("Posted image to yfrog!");
		qDebug("Url is %s", status.getMediaUrl().toLatin1().constData());
		if (m_message.length() > 0)
		{
			m_message += " ";
		}

		m_message += status.getMediaUrl();
		qDebug("Posting link to twitter..");
		postMessage();
	}
}



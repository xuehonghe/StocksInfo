#include "datamanager.h"

#include "qcustomplot.h"
#include "stock.h"

//#include "downloadmanager.h"



DataManager::DataManager(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<RealTimeQuoteData>("RealTimeQuoteData");

    m_allStocks = new QMap<QString, Stock*>();
    readStocksList();

//    m_downloadManager = new DownloadManager(this);
//    connect(m_downloadManager, SIGNAL(dataDownloaded(const QString &, const QUrl &)), this, SLOT(dataDownloaded(const QString &, const QUrl &)));

    m_localSaveDir = QApplication::applicationDirPath()+"/data";
    qDebug()<<"DataManager0:"<<QThread::currentThreadId();

}

DataManager::~DataManager(){
    qDebug()<<"DataManager::~DataManager()";

//    delete m_ohlcData; //deleted by QCPFinancial
//    delete m_tradeExtraData;
//    delete m_downloadManager;

    qDeleteAll(m_allStocks->begin(), m_allStocks->end());
    delete m_allStocks;

}

Stock * DataManager::stock(const QString &code) const{
    Stock *stock = m_allStocks->value(code);
    if(!stock){
        qDebug()<<QString("Stock '%1' Not Found!").arg(code);
    }
    return stock;
}

QMap<QString, Stock*> * DataManager::allStocks() const{
    return m_allStocks;
}

bool DataManager::readHistoricalData(QString *code, int offset){
    qDebug()<<"DataManager-readHistoricalData:"<<QThread::currentThreadId();

    QMutexLocker locker(&mutex);

    if(!code){return false;}
    QString newCode = *code;

    int size = m_allStocks->size();
    int newOffset = offset%size;
    if(!m_allStocks->contains(newCode)){return false;}
    if(newOffset){
        QStringList allStocks = m_allStocks->keys();
        int index = allStocks.indexOf(newCode);
        index += newOffset;

        if(newOffset > 0 && index > size){
            index = newOffset - (size - allStocks.indexOf(newCode));
        }else if(index < 0){
            index = size - (newOffset - allStocks.indexOf(newCode)) - 1;
        }
        newCode = allStocks.at(index);
        *code = newCode;
    }

    QFile file(m_localSaveDir+"/"+newCode+".csv");
    if(!file.exists()){
        downloadHistoricalData(newCode);
        return false;
    }
    if(!file.open(QIODevice::ReadOnly|QIODevice::Text)){
        qDebug()<<file.errorString();
        return false;
    }

    Stock *stock = m_allStocks->value(newCode);
    if(!stock){
        stock = new Stock(newCode, "");
        m_allStocks->insert(newCode, stock);
    }

    QTextStream in(&file);
    //表头
    QString title = in.readLine();
    if(title.trimmed().isEmpty()){
        return false;
    }
    qDebug()<<title;

    QMap<double, QCPFinancialData> *ohlcDataMap = stock->ohlcDataMap();
    QMap<double, TradeExtraData>  *tradeExtraDataMap = stock->tradeExtraDataMap();
    QVector<double> *futuresDeliveryDates = stock->futuresDeliveryDates();

    QVector< double >timeVector, openVector, highVector, lowVector, closeVector, volVector;
    uint index = (std::numeric_limits<uint>::max)();
    while (!in.atEnd()) {
        QStringList dataList = in.readLine().split(",");
        if(dataList.size()<12){
            qDebug()<<"Invalid column count!";
            return false;
        }

        double open = dataList.at(6).toDouble();
        if(isZero(open)){continue;} //停牌
        openVector.append(open);

        QDateTime dateTime = QDateTime::fromString(dataList.at(0), "yyyy-MM-dd");
        dateTime.setTime(QTime(15, 0));
        uint time_t = dateTime.toTime_t();
        QDate date = dateTime.date();
        if(date.dayOfWeek() == 5){
            //Futures delivery,期指交割日，忽略放假顺延
            QDate firstDay = date.addDays(-(date.day()-1));
            int firstFridy = 0;
            int dayofWeek = firstDay.dayOfWeek();
            if(dayofWeek > 5){
                firstFridy = 7-dayofWeek+5+1;
            }else{
                firstFridy = 5-dayofWeek+1;
            }
            if(date.day() == firstFridy+14){
                futuresDeliveryDates->append(index);
            }
        }


        double high = dataList.at(4).toDouble();
        highVector.append(high);

        double low = dataList.at(5).toDouble();
        lowVector.append(low);

        double close = dataList.at(3).toDouble();
        closeVector.append(close);


        double preClose = dataList.at(7).toDouble();

        double volume_Hand = dataList.at(11).toDouble();
        volVector.append(volume_Hand);

        double turnover = dataList.at(12).toDouble();
        double turnoverRate = dataList.at(10).toDouble();

        ohlcDataMap->insert(index, QCPFinancialData(index, open, high, low, close));
        tradeExtraDataMap->insert(index, TradeExtraData(time_t, preClose, volume_Hand, turnover, turnoverRate));

        //数据文件为倒序。不可使用日期做KEY，日期有空档。
        index--;
        qApp->processEvents();
    }

//    emit historicalDataRead(newCode);
    emit historicalDataRead(stock);

    return true;
}

void DataManager::downloadHistoricalData(const QString &code){
    qDebug()<<"-----DataManager::downloadData(...)"<<" currentThreadId:"<<QThread::currentThreadId();

    //m_downloadManager->append(QUrl(QString("http://quotes.money.163.com/service/chddata.html?code=%1&start=20000720&end=20150508")));

    QString url = QString("http://quotes.money.163.com/service/chddata.html?code=%1%2").arg(code.startsWith("6")?"0":"1").arg(code);
    emit requestDownloadData(url);

//    m_downloadManager->append();
}

void DataManager::historicalDataDownloaded(const QString &fileName, const QUrl &url){
    qDebug()<<"---DataManager::dataDownloaded(...)"<<" fileName:"<<fileName<<" currentThreadId:"<<QThread::currentThreadId();
    QFileInfo fi(fileName);
    QString code = fi.baseName();

    readHistoricalData(&code, 0);
}

void DataManager::downloadRealTimeQuoteData(const QString &code){
    //API:http://api.money.126.net/data/feed/1000001,money.api
    QString url = QString("http://api.money.126.net/data/feed/%1%2,money.api").arg(code.startsWith("6")?"0":"1").arg(code);
    emit requestRealTimeQuoteData(url);
}

void DataManager::realTimeQuoteDataReceived(const QByteArray &data){
    //API:http://api.money.126.net/data/feed/1000001,money.api
    if(data.isEmpty()){return;}

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if(error.error != QJsonParseError::NoError){
        qCritical()<<error.errorString();
        return;
    }
    QJsonObject object = doc.object();
    if(object.isEmpty()){return;}

    QString code = object["symbol"].toString();
    Stock *stock = m_allStocks->value(code);
    if(!stock){return;}

    RealTimeQuoteData rtData;
    rtData.code = code;
    rtData.time = object["time"].toString();
    rtData.ask1 = object["ask1"].toDouble();
    rtData.ask2 = object["ask2"].toDouble();
    rtData.ask3 = object["ask3"].toDouble();
    rtData.ask4 = object["ask4"].toDouble();
    rtData.ask5 = object["ask5"].toDouble();
    rtData.askVol1 = object["askvol1"].toDouble();
    rtData.askVol2 = object["askvol2"].toDouble();
    rtData.askVol3 = object["askvol3"].toDouble();
    rtData.askVol4 = object["askvol4"].toDouble();
    rtData.askVol5 = object["askvol5"].toDouble();
    rtData.bid1 = object["bid1"].toDouble();
    rtData.bid2 = object["bid2"].toDouble();
    rtData.bid3 = object["bid3"].toDouble();
    rtData.bid4 = object["bid4"].toDouble();
    rtData.bid5 = object["bid5"].toDouble();
    rtData.bidVol1 = object["bidvol1"].toDouble();
    rtData.bidVol2 = object["bidvol2"].toDouble();
    rtData.bidVol3 = object["bidvol3"].toDouble();
    rtData.bidVol4 = object["bidvol4"].toDouble();
    rtData.bidVol5 = object["bidvol5"].toDouble();

    RealTimeStatisticsData * statisticsData = stock->realTimeStatisticsData();
    statisticsData->open = object["open"].toDouble();
    statisticsData->high = object["high"].toDouble();
    statisticsData->low = object["low"].toDouble();
    statisticsData->price = object["price"].toDouble();
    statisticsData->change = object["updown"].toDouble();
    statisticsData->changePercent = object["percent"].toDouble();
    statisticsData->yestClose = object["yestclose"].toDouble();
    statisticsData->volume = object["volume"].toDouble();
    statisticsData->turnover = object["turnover"].toDouble();

    emit realTimeAskDataUpdated(rtData);

}

void DataManager::downloadRealTimeStatisticsData(const QString &code){
   //API:http://quotes.money.163.com/hs/service/diyrank.php?page=0&query=STYPE%3AEQA&fields=NO%2CSYMBOL%2CNAME%2CPRICE%2CPERCENT%2CUPDOWN%2CFIVE_MINUTE%2COPEN%2CYESTCLOSE%2CHIGH%2CLOW%2CVOLUME%2CTURNOVER%2CHS%2CLB%2CWB%2CZF%2CPE%2CMCAP%2CTCAP%2CMFSUM%2CMFRATIO.MFRATIO2%2CMFRATIO.MFRATIO10%2CSNAME%2CCODE%2CANNOUNMT%2CUVSNEWS&sort=SYMBOL&order=desc&count=24&type=query
    QString url = QString("http://api.money.126.net/data/feed/%1%2,money.api").arg(code.startsWith("6")?"0":"1").arg(code);
    emit requestRealTimeStatisticsData(url);
}

void DataManager::realTimeStatisticsDataReceived(const QByteArray &data){
    if(data.isEmpty()){return;}

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if(error.error != QJsonParseError::NoError){
        qCritical()<<error.errorString();
        return;
    }
    QJsonObject object = doc.object();
    if(object.isEmpty()){return;}

    QString time = object["time"].toString();
    QJsonArray array = object["list"].toArray();
    for(int i=0;i<array.size();i++){
        QJsonObject infoObj = array.at(i).toObject();
        if(infoObj.isEmpty()){continue;}

        QString code = infoObj["SYMBOL"].toString();
        QString name = infoObj["SNAME"].toString();
        Stock *stock = m_allStocks->value(code);
        if(!stock){
            stock = new Stock(code, name, this);
            m_allStocks->insert(code, stock);
        }

        RealTimeStatisticsData *statisticsData = stock->realTimeStatisticsData();
        statisticsData->time = time;
        statisticsData->open = object["OPEN"].toDouble();
        statisticsData->high = object["HIGH"].toDouble();
        statisticsData->low = object["LOW"].toDouble();
        statisticsData->price = object["PRICE"].toDouble();
        statisticsData->change = object["UPDOWN"].toDouble();
        statisticsData->changePercent = object["PERCENT"].toDouble();
        statisticsData->yestClose = object["YESTCLOSE"].toDouble();
        statisticsData->volume = object["VOLUME"].toDouble();
        statisticsData->turnover = object["TURNOVER"].toDouble();
        statisticsData->exchangeRatio = object["HS"].toDouble();
        statisticsData->tradableMarketCap = object["MCAP"].toDouble();
        statisticsData->marketCap = object["TCAP"].toDouble();
        statisticsData->pe = object["PE"].toDouble();
        statisticsData->earnings = object["MFSUM"].toDouble();
        statisticsData->volChangeRatio = object["LB"].toDouble();
        statisticsData->orderChangeRatio = object["WB"].toDouble();
        statisticsData->fiveMinsChange = object["FIVE_MINUTE"].toDouble();

    }

}

void DataManager::readStocksList(){
    QMutexLocker locker(&mutex);


    qDeleteAll(m_allStocks->begin(), m_allStocks->end());
    m_allStocks->clear();

    QFile file(QApplication::applicationDirPath()+"/data/AllStocks.csv");
    if(!file.open(QIODevice::ReadOnly|QIODevice::Text)){
        qDebug()<<file.errorString();
        //For Test
        m_allStocks->insert("000001", new Stock("000001", "\345\271\263\345\256\211\351\223\266\350\241\214"));
        m_allStocks->insert("000002", new Stock("000002", "\344\270\207  \347\247\221\357\274\241"));
        return;
    }

    QTextStream in(&file);
    while (!in.atEnd()) {
        QStringList dataList = in.readLine().split(",");
        if(dataList.size()!=2){
            continue;
        }
        m_allStocks->insert(dataList.at(0), new Stock(dataList.at(0), dataList.at(1)));
    }

    if(m_allStocks->isEmpty()){
        //For Test
        m_allStocks->insert("000001", new Stock("000001", "\345\271\263\345\256\211\351\223\266\350\241\214"));
        m_allStocks->insert("000002", new Stock("000002", "\344\270\207  \347\247\221\357\274\241"));
    }

    emit stocksLoaded(m_allStocks);

}






#include "app/player.h"
#include "app/ui_player.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QScreen>
#include <QPaintEvent>
#include <QPainter>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QGuiApplication>   // 只在 Wayland 才需要请系统接管窗口缩放
#include <QWindow>           // startSystemResize / startSystemMove
#include <QInputDialog>
#include <QDebug>
#include <QDateTime>          // QDateTime::currentMSecsSinceEpoch：进度条"钉住"/m_canSeek 的截止时刻
#include "platform/audioplayer.h"
#include "platform/videowidget.h"

// CMD_OPEN_OBJ 已移到 core/messagequeue.h —— PlayerCore 处理 CMD_OPEN 时也要按它解析

//快进/快退的“尾部”去抖窗口：停止按键多久后，把累积的目标补上。
//配合“前沿”（这一串按键的第一次立即生效）实现 B 站式手感：
//第一次按下马上有反馈，连按/长按只累积，静默 kSeekDebounceMs 后再补一次最终位置。
static constexpr int kSeekDebounceMs = 120;

//进度条“钉在目标上”的最长时间（毫秒）：seek 落地会提前解除，这个只是兜底，
//防止 EVT_SEEK_DONE 丢了把进度条永久冻在目标上。
static constexpr int kSeekUiHoldMs = 1500;
//判定“seek 已经落地”的容差（毫秒）：真实时钟走到目标 ±这么多就算到位了。
//取 1s：视频 seek 对齐关键帧可能偏一点点，1s 足够；而连按 5s 一档，不会误判成上一发的落点。
static constexpr long long kSeekLandToleranceMs = 1000;
//m_canSeek 的兜底：发出 seek 后最多锁这么久，到点自动解锁（SEEK_DONE 正常会提前解锁）。
//取 2s：比一次普通本地 seek 慢不少，但又不至于让快进键按了没反应太久。
static constexpr int kSeekLockTimeoutMs = 2000;
//“用户正在连续 seek”的窗口（毫秒）：窗口内收到的 SEEK_DONE 不去改进度条。
//取 1.2s：比一次按键间隔 + 一次 seek 落地的时间都长，手松开后自动退出该状态。
static constexpr int kSeekActiveWindowMs = 1200;

/*
 * 诊断开关：设置环境变量 QTPLAYER_SEEK_DEBUG=1 时，把 seek / 进度条的关键数值
 * 打到 Application Output（平时关闭，不影响正常运行）。
 * 排查"进度条来回跳"这类问题时很有用：能看到每次按键用的 base、发出去的目标，
 * 以及每次 EVT_POSITION 带来的时钟值。
 */
static bool seekDebugOn()
{
    static const bool on = qEnvironmentVariableIsSet("QTPLAYER_SEEK_DEBUG");
    return on;
}

Player::Player(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Player)
{
    ui->setupUi(this);
    m_widget = ui->video;
    m_auPlayer = new AudioPlayer();

    if(!m_widget || !m_auPlayer){
        qDebug()<<"PlayerCore Init failed";
        exit(0);
    }

    //设置窗口图标&默认窗口标题
    this->setWindowIcon(QIcon(":/workBtnPNG/OPlayer.png"));
    this->setWindowTitle("OPlayer");
    //去掉标题和原生按钮
    this->setWindowFlags(Qt::FramelessWindowHint);
    // 设置VideoWidget大小策略为自动扩展
    ui->video->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // 初始就铺满整个窗口
    resize(1280+12, 720+ui->ctrlbar->height()+ui->topMenu->height()+12);              // 默认窗口大小（更友好）
    setMinimumSize(432+12, 243+ui->ctrlbar->height()+ui->topMenu->height()+12);       // 最小限制（16:9）
    ui->video->setFixedSize(1280, 720);

    //边缘缩放  给所有可能抢焦点的子控件装过滤器
    ui->video->installEventFilter(this);
    ui->ctrlbar->installEventFilter(this);
    ui->playList->getListWidget()->installEventFilter(this);

    ui->video->setGeometry(0, 0, width(), height());

    //---------------------------------------------------------------------------
    //播放列表初始化
    ui->dockWidget->setFeatures(QDockWidget::NoDockWidgetFeatures);
    ui->dockWidget->setFixedWidth(m_playListWidth);
    ui->dockWidget->hide();
    ui->dockWidget->setStyleSheet("QDockWidget::title { height:0; }");
    ui->dockWidget->setFloating(false);
    ui->dockWidget->setAllowedAreas(Qt::NoDockWidgetArea);
    ui->dockWidget->setWindowTitle("播放列表");

    //---------------------------------------------------------------------------
    //底部控制按键组信号操作
    //开始与暂停按钮控制
    connect(ui->ctrlbar, &CtrlBar::play, this,&Player::play);
    connect(ui->ctrlbar, &CtrlBar::pause, this,&Player::pause);
    connect(ui->ctrlbar, &CtrlBar::setVolume, this,&Player::setVolume);
    connect(this,&Player::setPausePicture,ui->ctrlbar, &CtrlBar::setPausePictrue);
    //停止播放
    connect(ui->ctrlbar, &CtrlBar::stop, this,&Player::stopToPlay);
    //进度条移动和seek操作
    connect(ui->ctrlbar, &CtrlBar::seek, this,&Player::sliderSeek);
    //全屏
    connect(ui->ctrlbar, &CtrlBar::fullPlay, this, &Player::toggleFullScreen);
    //打开播放列表
    connect(ui->ctrlbar, &CtrlBar::showOrHidePlayList, this, &Player::showOrHidePlayList);
    //打开设置

    //下一集
    connect(ui->ctrlbar, &CtrlBar::next, this, &Player::playNext);
    //上一集
    connect(ui->ctrlbar, &CtrlBar::prev, this, &Player::playPrev);
    //快进5s
    connect(ui->ctrlbar, &CtrlBar::ff, this, &Player::ffSeekFiveSec);
    //快退5s
    connect(ui->ctrlbar, &CtrlBar::rewind, this, &Player::rewindSeekFiveSec);
    //下一帧
    connect(ui->ctrlbar, &CtrlBar::nextFrame, this, &Player::stepFrame);
    //上一帧
    connect(ui->ctrlbar, &CtrlBar::prevFrame, this, &Player::stepFrame);
    //倍速
    connect(ui->ctrlbar, &CtrlBar::speedChanged, this, &Player::changeSpeed);
    //滤镜切换
    connect(ui->ctrlbar, &CtrlBar::filterChanged, this, [this](int type){
        ui->video->setFilterType(type);
    });


    //---------------------------------------------------------------------------
    //顶部按键组
    connect(ui->topMenu, &TopMenu::openFile, this,&Player::on_openFile);
    connect(ui->topMenu, &TopMenu::openLink, this,&Player::on_openLink);
    connect(ui->topMenu, &TopMenu::closeClicked, this, &QWidget::close);
    connect(ui->topMenu, &TopMenu::hideWindow, this,[this]{this->showMinimized();});
    connect(ui->topMenu, &TopMenu::maximization, this,[this]{
        m_normalGeo = this->geometry();
        setGeometry(screen()->availableGeometry());
        m_isMaximum = true;
        m_maximunFlag = true;});
    connect(ui->topMenu, &TopMenu::restore, this,[this]{
        this->setGeometry(m_normalGeo);
        m_isMaximum = false;
        m_maximunFlag = true;
    });
    connect(this,&Player::setPlayingText,ui->topMenu, &TopMenu::setPlayingText);
    connect(this,&Player::maxOrRestore,ui->topMenu, &TopMenu::maxOrRestoreChange);




    //---------------------------------------------------------------------------
    //播放列表

    //播这个
    connect(ui->playList->getListWidget(), &QListWidget::itemDoubleClicked,
            this, &Player::onPlayList_doubleClicked);
    connect(ui->playList, &PlayList::playThis,
            this, &Player::onPlayList_doubleClicked);



    // 播放列表只作为 UI 的"下一集"来源；core 不再关心播放列表（CTX_HAS_PLAYLIST 已删），
    // 播完由 OP_EVENT_COMPLETED 触发 playNext()。

    //支持拖文件进来
    setAcceptDrops(true);

    //SDK调用
    // 1. 创建播放器
    m_ctx.player = op_create();
    m_ctx.self   = this;
    if (!m_ctx.player) {
        qDebug()<<"创建播放器失败";
        exit(0);
    }

    // 2. 注册回调（静态成员函数可以隐式转成 C 函数指针）
    op_set_event_cb(m_ctx.player, &Player::on_event, &m_ctx);
    op_set_video_cb(m_ctx.player, &Player::on_video, &m_ctx);
    op_set_log_cb(m_ctx.player,  &Player::on_log,  &m_ctx);

    // 3. 配置音频（必须在 open 之前！）
    op_set_audio_format(m_ctx.player, 48000, 2);

    /*
     * 音频设备**不在这里打开**：Player 构造时 QApplication 的事件循环还没跑，
     * QAudioSink::start() 会停在 IdleState、bufferSize 一个字节都不消耗
     * （实测 bytesFree 永远等于 bufferSize），写进去的 PCM 全堵在设备里。
     * 改成第一次 playFile 时再 open（那时事件循环已经在跑）。
     */
}

Player::~Player()
{
    if (m_seekDebounceTimerId) { killTimer(m_seekDebounceTimerId); m_seekDebounceTimerId = 0; }

    /*
     * 顺序很重要（三条都不能颠倒）：
     *   1) 先停音频：QAudioSink 的 readData 正在调 op_audio_read，
     *      player 释放后再回调就是 use-after-free；
     *   2) 再 op_stop + op_destroy：stop 会 join 消息循环和 demux/解码/渲染/音频线程，
     *      返回后不再有任何 EVT_* / 视频帧回调；
     *   3) 最后拆 UI（delete ui）。
     * 反过来任何一步都可能让后台线程回调进已经析构的对象，表现为退出时随机崩溃。
     */
    if (m_auPlayer) m_auPlayer->close();

    if (m_ctx.player) {
        op_stop(m_ctx.player);
        op_destroy(m_ctx.player);
        m_ctx.player = nullptr;
    }

    delete m_auPlayer;   // ~AudioPlayer 里再 close 一次（幂等）
    m_auPlayer = nullptr;

    delete ui;
    ui = nullptr;
}

void Player::resizeEvent(QResizeEvent *e)
{
    if(m_isMaximum && !m_maximunFlag){
        emit maxOrRestore();
        m_isMaximum = false;
    }
    m_lastW = this->width();
    m_lastH = this->height();
    QWidget::resizeEvent(e);
    if(is_playListVisible && !m_maximunFlag){
        if(m_listBtnOn && !m_maximunFlag){
            m_listBtnOn  = false;
            return;
        }
    }
    m_listBtnOn  = false;
    m_maximunFlag = false;
    int playW = this->width() -12;
    if(is_playListVisible){
        playW -= m_playListWidth;
    }
    int playH = this->height()
                - ui->topMenu->height()
                - ui->ctrlbar->height()-12;
    if (playW <= 0 || playH <= 0){
        ui->video->setFixedSize(0, 0);
        return;
    };
    double srcAspect;
    if (m_videoSrcW <= 0 || m_videoSrcH <= 0){
        srcAspect = (double)16/9;
    }else{
         srcAspect = (double)m_videoSrcW / m_videoSrcH;
    }
    double boxAspect = (double)playW / playH;
    int newW, newH;
    if (boxAspect > srcAspect) {
        newH = playH;
        newW = playH * srcAspect;
    } else {
        newW = playW;
        newH = playW / srcAspect;
    }
    m_lastVideoW = newW;
    m_lastVideoH = newH;
    ui->video->setFixedSize(newW, newH);
}

void Player::mouseDoubleClickEvent(QMouseEvent *e)
{
   toggleFullScreen();
}



void Player::on_openFile()
{
    QString name = QFileDialog::getOpenFileName(this, "选择视频文件");
    if (name.isEmpty()) return;
    //addToPlayList(name);   // 打开也加入列表
    playFile(name);
}

void Player::on_openLink()
{
    bool ok = false;
    QString url = QInputDialog::getText(
        this,
        "打开链接",
        "请输入视频链接（RTMP):",
        QLineEdit::Normal,
        "",
        &ok
        );

    if (!ok || url.isEmpty())
        return;

    if (url.startsWith("rtmp://") || url.startsWith("http://") || url.startsWith("https://")) {
        playFile(url);
    } else {
        QMessageBox::warning(this, "错误", "不支持的链接格式，请使用 RTMP/HTTP/HTTPS。");
    }
}

void Player::setVolume(double pos)
{
    // 音量直接作用于 Player 自己持有的 AudioPlayer。
    m_nowVolume = pos;
    if (m_auPlayer) m_auPlayer->setVolume(pos);
}

void Player::toggleFullScreen()
{
    if(m_videoSrcW == 0 || m_videoSrcH == 0 ){
        return;
    }
    if (!m_isFullScreen) {
        // 进入全屏
        m_isFullScreen = true;
        ui->topMenu->hide();
        ui->ctrlbar->hide();
        ui->dockWidget->hide();
        showFullScreen();
        //抹去间隔
        this->layout()->setContentsMargins(0,0,0,0);
        ui->video->setFixedSize(m_videoSrcW,m_videoSrcH);

    } else {
        // 退出全屏
        m_isFullScreen = false;
        ui->topMenu->show();
        ui->ctrlbar->show();
        //添加间隔以便缩放
        this->layout()->setContentsMargins(6,6,6,6);
        showNormal();
        ui->video->setFixedSize(m_lastVideoW,m_lastVideoH);
        if (is_playListVisible) {
            ui->dockWidget->show();
        }
    }
}

void Player::stopToPlay()
{
    if(!m_isInit) return;
    exitStepFrame();
    setPausePicture(true);
    //停机：seek 相关状态一起复位，免得下次打开时快进/快退还锁着、进度条还钉着旧目标
    m_pendingSeekMs     = -1;
    m_seekTargetMs      = -1;
    m_seekUiHoldUntilMs = 0;
    m_canSeek           = true;
    m_canSeekUntilMs    = 0;
    op_stop(m_ctx.player);
    if (m_auPlayer) {
        m_auPlayer->setPause(true);
        m_auPlayer->clear();      // 丢掉设备里残留的上一段 PCM
    }
    m_widget->clearScreen();
    //停机后不再有"流媒体"状态，这些机制恢复默认可用（下次 playFile 会按新片源重新判定）
    ui->ctrlbar->setStreamMode(false);
    ui->ctrlbar->setSliderMaximum(0);
    ui->ctrlbar->setSliderValue(0);
    ui->ctrlbar->setDisabled(true);
    ui->topMenu->setNoPlayText();
    m_videoSrcH = 0;
    m_videoSrcW = 0;
    m_isInit = false;
    m_widget->clearScreen();
}



void Player::showOrHidePlayList()
{
    m_listBtnOn = true;
    if (is_playListVisible) {
        ui->dockWidget->hide();
        is_playListVisible = !is_playListVisible;
        setMinimumSize(432+12, 243+ui->ctrlbar->height()+ui->topMenu->height()+12);
        if(m_isMaximum){
            this->showMaximized();
        }else{
            resize(m_lastW-m_playListWidth,m_lastH);
        }
    } else {
        is_playListVisible = !is_playListVisible;
        setMinimumSize(432+12+m_playListWidth, 243+ui->ctrlbar->height()+ui->topMenu->height()+12);
        //int newW = this->width() + m_playListWidth;
        int newW = m_lastW + m_playListWidth;
        if(m_lastVideoW <= 432){
            newW = m_lastW;
        }

        int newH = this->height();
        if(m_isMaximum){
            newW = this->width();
            ui->dockWidget->show();
            resize(newW,newH);
        }else{
            resize(newW,newH);
            ui->dockWidget->show();
        }
    }
}

void Player::playFile(const QString &path)
{

    if (path.isEmpty()) return;

    //换文件前先退出逐帧
    exitStepFrame();

    op_open_options opt{};
    opt.open_timeout_ms = 15000;
    opt.fd = -1;
    std::string pathStr = path.toStdString();
    opt.path = pathStr.c_str();
    op_result res = op_open(m_ctx.player,&opt);
    if (res != OP_OK) {
        // 处理打开失败
        return;
    }

    /*
     * 音频设备在**第一次打开文件时**才启动：
     * 构造函数里 QApplication 的事件循环还没跑，QAudioSink 会停在 IdleState
     * （bytesFree 永远是满的），PCM 写进去没人消费 → 环形缓冲堵满 →
     * 音频时钟不前进 → 视频播一下就冻住。
     */
    if (!qEnvironmentVariableIsSet("OP_QT_NO_AUDIO") && !m_auPlayer->isOpen()) {
        if (!m_auPlayer->open(48000, 2, m_ctx.player))
            qDebug() << "音频设备打开失败，本次播放将没有声音";
    }
    // 丢掉上一轮残留的 PCM，再恢复播放（换文件时设备里可能还压着上一个文件的一小段）
    if (m_auPlayer->isOpen()) {
        m_auPlayer->clear();
        m_auPlayer->setPause(false);
    }

    emit setPlayingText(path);

}

void Player::playNext()
{
    //流媒体不支持上下集（播完也不会自动切，见 DemuxThread::run 的 EOF 分支）
    if(m_isStream) return;
    QMetaObject::invokeMethod(this,[this](){
        QListWidget *list = ui->playList->getListWidget();
        if (list->count() == 0) return;
        ui->playList->next();                              // 环形选中下一行
        QString path = ui->playList->itemPath(ui->playList->currentRow());
        if (!path.isEmpty()) playFile(path);
    },Qt::QueuedConnection);
}

void Player::playPrev()
{
    //流媒体不支持上下集
    if(m_isStream) return;
    QMetaObject::invokeMethod(this,[this](){
        QListWidget *list = ui->playList->getListWidget();
        if (list->count() == 0) return;
        ui->playList->prev();                              // 环形选中上一行
        QString path = ui->playList->itemPath(ui->playList->currentRow());
        if (!path.isEmpty()) playFile(path);
    },Qt::QueuedConnection);

}

void Player::onPlayList_doubleClicked(QListWidgetItem *item)
{
    if (!item) return;
    ui->playList->setCurrentRow(ui->playList->getListWidget()->row(item));
    QString path = item->data(Qt::UserRole).toString();
    if (!path.isEmpty()) playFile(path);
}

void Player::ffSeekFiveSec()
{
    if(m_stepFrame) return;
    queueSeekBy(+5000);
}

void Player::rewindSeekFiveSec()
{
    if(m_stepFrame) return;
    queueSeekBy(-5000);
}

// 快进/快退统一入口：前沿立即生效 + 尾部去抖收尾。
//   前沿：这一串按键的第一次立即 seek（消除“按了要等”的延迟感）
//   尾部：窗口内的后续按键只累积到 m_pendingSeekMs，静默 kSeekDebounceMs 后
//         由 timerEvent 补一次最终位置；只按一次时它是空的，收尾会自动跳过。
// 因此：单按 = 1 次 seek，连按/长按 = 2 次 seek（而不是 N 次）。
void Player::queueSeekBy(long long deltaMs)
{
    //流媒体不支持快进/快退（DemuxThread::requestSeekMs 里还有一道闸门兜底）
    //if(!m_isInit || dt.isStreamMedia() || dt.m_seekPauseing.load()) return;
    if(!m_isInit || m_isStream) return;
    //即使这一次因为“上一次 seek 还没落地”被丢弃，也要记下“用户还在连续操作”：
    //否则窗口会过期，随后落地的 SEEK_DONE 就会去抢进度条，造成按键/落地两套值互相顶。
    m_seekActiveUntilMs = QDateTime::currentMSecsSinceEpoch() + kSeekActiveWindowMs;
    if(m_isStream || !m_canSeek) return;

    //逐帧中快进/快退：先退出逐帧（位置会对齐）
    exitStepFrame();

    const long long totalMs = m_durationMs;
    if(totalMs <= 0) return;   //时长未知，没法和范围对齐

    //基准：有“已累积但还没生效”的目标就接着累积；否则若进度条还钉在某个目标上
    //（seek 已发出但还没落地），也必须接着那个目标加 —— 这段窗口里 m_pts 还是旧时钟，
    //拿它当基准会把连按的第二下算丢。
    const long long base = (m_pendingSeekMs >= 0) ? m_pendingSeekMs
                         : (m_seekTargetMs   >= 0) ? m_seekTargetMs
                                                   : m_pts;
    long long target = base + deltaMs;
    if(target < 0) target = 0;
    if(target > totalMs) target = totalMs;

    if (seekDebugOn())
        qDebug() << "[seek] by" << deltaMs << "base" << base << "target" << target
                 << "| pts" << (long long)m_pts << "pin" << m_seekTargetMs
                 << "pending" << m_pendingSeekMs << "canSeek" << m_canSeek
                 << "timer" << m_seekDebounceTimerId;

    //进度条 + 时间文字立刻跟到目标（setSliderValue 内部会同步 playSlider 和 playTimeEdit）
    ui->ctrlbar->setSliderValue((int)target);

    if(m_seekDebounceTimerId == 0){
        //前沿：这一串按键的第一次立即生效（postSeek 里会钉住目标并串行化）
        m_pendingSeekMs = -1;
        postSeek(target);
    }else{
        //后续按键：只累积，等静默后再 seek 末次
        m_pendingSeekMs = target;
        //显示先跟到目标（不发包），这样长按时进度条是一格一格往前走的
        pinSeekTarget(target);
    }

    //尾部：每次按键都重置，静默 kSeekDebounceMs 后由 timerEvent 收尾
    if(m_seekDebounceTimerId) killTimer(m_seekDebounceTimerId);
    m_seekDebounceTimerId = startTimer(kSeekDebounceMs);
}

// 把进度条钉在目标上：从这一刻起到 seek 落地（或超时）之前，EVT_POSITION 不许改滑块。
// 同时记住目标值，连按累积时要用它当基准（这段时间 m_pts 还是旧时钟）。
void Player::pinSeekTarget(long long targetMs)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_seekTargetMs      = targetMs;
    m_seekUiHoldUntilMs = now + kSeekUiHoldMs;
    //用户刚刚操作过：把“连续 seek 窗口”推到未来，期间 SEEK_DONE 不许改进度条
    m_seekActiveUntilMs = now + kSeekActiveWindowMs;
}

// 所有 seek 请求的唯一出口：显示目标 → 钉住 → 串行化 → 投递。
// 之所以收在一处：以前前沿 / 去抖尾部 / 拖进度条三个地方各自 postMessage，
// 尾部那条既没检查 m_canSeek 也没把它置 false，串行化形同虚设。
void Player::postSeek(long long targetMs)
{
    ui->ctrlbar->setSliderValue((int)targetMs);
    pinSeekTarget(targetMs);
    m_canSeek        = false;
    m_canSeekUntilMs = QDateTime::currentMSecsSinceEpoch() + kSeekLockTimeoutMs;
    op_seek_ms(m_ctx.player,targetMs);
}



void Player::stepFrame(int mode)
{
    //未初始化和纯音频文件不能逐帧
    if(!m_isInit ||op_is_music(m_ctx.player)) return;
    //流媒体没有"上一帧/下一帧"这个概念（底层也回退不了），不进入逐帧模式
    if(op_is_stream(m_ctx.player)) return;
    /*
     * 纯音频没有帧可逐。正常情况下按钮已经被 CtrlBar::setFrameStepEnabled(false) 关掉了，
     * 这里再挡一道是因为 F/D 快捷键不经过按钮。
     * 挡住之后 UI 保持原状，不会切出“假的逐帧外观”（核心那边 stepNext/PrevFrame
     * 对无视频流本来也是直接返回 false）。
     */
    if(!op_has_video(m_ctx.player)) return;
    //上一帧 / 下一帧：进入逐帧模式（暂停渲染，解码继续）
    if(mode == 1){
        op_step_frame(m_ctx.player,1);
    }else{
        // dir 只有 +1 / -1 两种取值；传 2 会被 SDK 当非法参数直接拒绝
        op_step_frame(m_ctx.player,-1);
    }
    if(!m_stepFrame){
        m_stepFrame = true;
        m_isPause = true;
        //按钮切换到暂停外观，并锁掉逐帧期间不该用的按钮
        ui->ctrlbar->setPausePictrue(true);
        ui->ctrlbar->stepFrameTime(true);
        ui->topMenu->stepFrameTime(true);
    }
}

void Player::exitStepFrame()
{
    if(!m_stepFrame) return;
    m_stepFrame = false;
    ui->ctrlbar->stepFrameTime(false);
    ui->topMenu->stepFrameTime(false);
    op_exit_frame_step(m_ctx.player);
}

void Player::adjustVolume(double delta)
{
    if(!m_isInit) return;
    double vol = m_nowVolume + delta;
    if (vol > 1.0) vol = 1.0;
    if (vol < 0.0) vol = 0.0;
    m_nowVolume = vol;
    setVolume(vol);   // 会同步 UI 滑块和 dt.setVolume
    // 同步 CtrlBar 音量滑块显示
    ui->ctrlbar->setVolumeSlider(vol * 100);   // 需要 CtrlBar 提供这个方法
}

void Player::changeSpeed(double delta)
{
    //流媒体不支持倍速（DemuxThread::setSpeed 里还有一道闸门兜底）
    if(op_is_stream(m_ctx.player)) return;
    double newSpeed = m_speed + delta;
    if (newSpeed < 0.5) newSpeed = 0.5;
    if (newSpeed > 2.0)  newSpeed = 2.0;
    m_speed = newSpeed;

    /*
     * 下发用四舍五入，不要直接 double→int 截断：0.1 在二进制里是循环小数，
     * 累加几十次之后可能得到 1.6999999999999997 这种值，截断就变成 16（1.6x），
     * 于是“按了一下居然没变/跳了一档”。
     */
    op_set_speed(m_ctx.player,qRound(m_speed * 10.0));
    ui->ctrlbar->setSpeedLabel(op_get_speedX10(m_ctx.player));
}

void Player::onEVT_PREPARED(const op_event *ev)
{
    long long durationMs = op_duration_ms(m_ctx.player);
    int width = op_get_width(m_ctx.player);
    int height = op_get_height(m_ctx.player);
    bool isStream = op_is_stream(m_ctx.player);
    bool hasVideo = op_has_video(m_ctx.player);
    QMetaObject::invokeMethod(this,[this,width,height,durationMs,isStream,hasVideo](){
        this->m_videoSrcW = width;
        this->m_videoSrcH = height;          // 注意别写成 m_videoSrcW（height 为 0 时 Init 会把控件置成 0 高，setPaint 会丢掉所有帧）
        this->m_durationMs = durationMs;
        ui->ctrlbar->setSliderMaximum(durationMs);
        ui->ctrlbar->setSliderValue(0);
        m_widget->Init(m_videoSrcW,m_videoSrcH);

        //ui->video->setPerfStartNs(startNs);
        //emit setPlayingText(path);
        this->showNormal();
        m_isInit = true;
        /*
        * 流媒体：把快进/快退/逐帧/倍速/上下集这些入口整体关掉（按钮 + 快捷键 + 进度条），
        * 换回本地视频时这里会重新判定并自动恢复。
        */
        ui->ctrlbar->setStreamMode(isStream);
        //纯音频没有帧可逐：直接把逐帧入口关掉（按钮置灰），别让用户点进去再被核心拒绝
        ui->ctrlbar->setFrameStepEnabled(hasVideo
                                         && !isStream);
        if (isStream && m_speed != 1.0) {
            //倍速已经被 DemuxThread::openFile 强制拉回 1.0，标签也得跟着回到 1.0，
            //否则界面写着 2.0x、实际按 1.0x 播，而倍速按钮又是灰的，用户无从判断。
            m_speed = 1.0;
            ui->ctrlbar->setSpeedLabel(m_speed);
        }
        // 重置滑块状态，避免之前操作的影响
        isSliderPress = false;                 // 清除按下标志
        // 换文件时丢弃上一次遗留的待生效 seek 与去抖定时器
        if (m_seekDebounceTimerId) {
            killTimer(m_seekDebounceTimerId);
            m_seekDebounceTimerId = 0;
        }
        m_pendingSeekMs = -1;
        //换文件：上一次遗留的“进度条钉住”和 seek 锁也一起作废
        m_seekTargetMs      = -1;
        m_seekUiHoldUntilMs = 0;
        m_canSeek           = true;
        m_canSeekUntilMs    = 0;
        ui->ctrlbar->setDisabled(false);
        setVolume(m_nowVolume);
        update();
        resizeEvent(nullptr);
        setPausePicture(m_isPause);
        if(!m_isPause){
            play();
        }else{
            pause();
        }
        if (!hasVideo) {
            ui->video->clearScreen();   // 纯音频：黑屏
        }

    },Qt::QueuedConnection);
}

void Player::onEVT_POSITION(const op_event *ev)
{
    // 用事件里带的位置（这一刻的值）；不要改成 op_position_ms() —— 回调排队到
    // GUI 线程后，查询接口可能已经被下一条 POSITION 覆盖了。
    int ms = (int)ev->position_ms;
    double speed = 1.0*op_get_speedX10(m_ctx.player) / 10.0;

    /*
     * 这里**不要**调 m_auPlayer->getNoPlayMs()：本回调跑在 SDK 的消息循环线程，
     * 而 m_auPlayer 的泵在 GUI 线程（QTimer）里也会读写同一个 QAudioSink。
     * QAudioSink 不是线程安全的 —— 两边同时动它，实测会偶发 0xc0000005（退出/播完时）。
     * 设备积压由 AudioPlayer::pump() 在 GUI 线程里每 10ms 报一次，这里不需要重复。
     */

    if (seekDebugOn()) qDebug() << "[seek] position" << ms;
    QMetaObject::invokeMethod(this,[this,ms,speed](){
        m_pts = ms;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        /*
         * 进度条钉在目标上时（用户刚 seek / 连按快进快退），别让真实时钟把它顶回去。
         * 解除条件二选一：
         *   1) 时钟已经走到目标附近 → seek 落地了，可以交还给时钟；
         *   2) 超过截止时间 → 兜底（SEEK_DONE 可能丢了），也交还，绝不永久冻结。
         */
        if (m_seekTargetMs >= 0) {
            const long long diff = (long long)ms - m_seekTargetMs;
            const bool   landed  = (diff < 0 ? -diff : diff) <= kSeekLandToleranceMs;
            if (!landed && now < m_seekUiHoldUntilMs) return;   // 还在等落地：显示保持目标
            m_seekTargetMs      = -1;
            m_seekUiHoldUntilMs = 0;
        }
        //兜底：SEEK_DONE 迟迟不来（丢了 / doSeek 早退）时解开快进/快退的锁，
        //否则 m_canSeek 会一直是 false，按键彻底没反应。
        if (!m_canSeek && now >= m_canSeekUntilMs) m_canSeek = true;

        ui->ctrlbar->setSliderValue(ms);
        ui->ctrlbar->setSpeedLabel(speed);
    },Qt::QueuedConnection);
}

void Player::onDISABLEBTN()
{
    QMetaObject::invokeMethod(this,[this](){
        /*
         * 只禁用控制栏，**不要碰暂停图标**，也**不要禁用顶栏**。
         *
         * 图标：这里读到的 m_playerCtx->getIsPause() 是异步广播，而 seek 期间核心会把
         * CTX_IS_PAUSE 临时置 1（为了先停生产）—— 拿它刷新图标就会把“正在播放”
         * 错判成“已暂停”：CtrlBar 的 m_isPause 被改成 true，而 Player::m_isPause 没变。
         * 两个源分叉后，第一次按空格/点播放会走反方向，表现为“要点两次才开始播”。
         * 暂停图标只由 play()/pause()/逐帧/打开文件改，它们都是用户动作，不会失真。
         *
         * 顶栏：里面有最小化/最大化/关闭和“打开文件”。seek 可能持续几百毫秒，全部禁用
         * 会让用户连窗口都关不掉；万一 seek 卡住（坏文件/网络盘），界面就没有任何出口了。
         * 要防的是“seek 期间再拖一次进度条”，那本来就属于控制栏。
         */
        ui->ctrlbar->setDisabled(true);
    },Qt::QueuedConnection);

}

void Player::onABLEBTN()
{   
    QMetaObject::invokeMethod(this,[this](){
        //同上：只恢复控制栏，不碰暂停图标和顶栏
        ui->ctrlbar->setDisabled(false);
    },Qt::QueuedConnection);
}

void Player::onEVT_ERROR(const op_event *ev)
{
    QString wrong = "未知错误";
    if (ev->message != nullptr) {
        const char *errStr = static_cast<const char *>(ev->message);
        wrong = QString::fromStdString(errStr);
    }
    QMetaObject::invokeMethod(this,[this,wrong](){
        QMessageBox::warning(this, "错误", wrong);
    },Qt::QueuedConnection);
}

void Player::onSEEkDONE(const op_event *ev)
{
    // 落点必须用事件里的 position_ms：回调排队到 GUI 线程后 op_position_ms()
    // 可能已经是"播放中的下一个位置"了（之前这里误用了 op_duration_ms）。
    int64_t ms = ev->position_ms;
    if (seekDebugOn()) qDebug() << "[seek] done" << ms << "pending" << m_pendingSeekMs;
    QMetaObject::invokeMethod(this,[this,ms](){
        //落地：解除“禁止再发 seek”
        m_canSeek           = true;
        m_canSeekUntilMs    = 0;

        //连按/拖动期间攒下的目标：上一次落地之后再补发，保证同一时刻只有一发在飞
        if (m_pendingSeekMs >= 0) {
            const long long target = m_pendingSeekMs;
            m_pendingSeekMs = -1;
            postSeek(target);      //内部会刷进度条 + 重新钉住
            return;
        }

        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        //内部位置先校正：下一次按键取累积基准时用得上（EVT_POSITION 随后也会覆盖它）
        m_pts = static_cast<long long>(ms);

        /*
         * 用户还在连续快进/快退（最近一次操作在 kSeekActiveWindowMs 之内）：
         * 进度条此时由按键驱动、已经显示在目标上了，这里**不要再拿落点去改它**。
         * 否则每按一下就会“目标 → 精准落点 → 下一个目标”写两次，看起来是抖的。
         * 显示最终由 EVT_POSITION 收敛：时钟走到钉住的目标附近时自动接管（超时也会兜底）。
         */
        if (now < m_seekActiveUntilMs) {
            if (seekDebugOn()) qDebug() << "[seek] done" << ms << "suppressed (user active)";
            return;
        }

        /*
         * 不在连续操作（单次 seek / 拖动松手后已静止）：把显示校正到真实落点。
         * 注意不要把 m_seekTargetMs 直接清成 -1 —— 清掉的话下一次按键会回退到 m_pts，
         * 而 m_pts 只在 EVT_POSITION 里更新，此刻可能还是 seek 之前的位置。
         */
        m_seekTargetMs      = static_cast<long long>(ms);
        m_seekUiHoldUntilMs = now + kSeekUiHoldMs;
        ui->ctrlbar->setSliderValue(ms);
    },Qt::QueuedConnection);
    //打开按钮
    onABLEBTN();
}

/*
 * 停止播放后投一帧纯黑，保证画面不残留最后一帧。
 *
 * 为什么是 Y=16 而不是 0：播放链路是 limited range（16~235），
 * "黑"对应 Y=16、U=V=128。填 0 在 limited 显示器上是溢出值，某些渲染路径会显示成异常色块。
 *
 * 顺序要求：必须在 clearScreen() 之后调用 —— 否则刚塞进来的黑帧会被清掉。
 * 拿到 widget 之后 setPaint() 会接管所有权（含尺寸不匹配时丢弃并 free）。
 */
void Player::postBlackFrame()
{
    const int w = m_videoSrcW > 0 ? m_videoSrcW : 1280;
    const int h = m_videoSrcH > 0 ? m_videoSrcH : 720;

    // YUV420P 紧凑无填充总大小
    int32_t y_size  = w * h;
    int32_t uv_size = (w/2) * (h/2);
    int32_t total_size = y_size + uv_size * 2;

    op_frame* black = op_frame_alloc(total_size);
    if (!black) return;

    black->format = OP_PIX_FMT_YUV420P;
    black->width  = w;
    black->height = h;

    black->size   = total_size;
    black->linesize[0] = w;
    black->linesize[1] = w / 2;
    black->linesize[2] = w / 2;
    black->linesize[3] = 0;

    uint8_t *Y = black->data;
    uint8_t *U = Y + y_size;
    uint8_t *V = U + uv_size;

    // 黑色：Y=16，U=128，V=128
    memset(Y, 16,  y_size);
    memset(U, 128, uv_size);
    memset(V, 128, uv_size);

    ui->video->beginPaint();        // 走既有的"最多一帧排队"名额
    ui->video->setPaint(black);     // 所有权交给 widget
    ui->video->endPaint();
}


void Player::sliderSeek(double pos)
{
    if(!m_isInit || m_stepFrame) return;
    //用户主动拖动：丢弃还没生效的累积目标，免得它晚一步把进度拽回去
    if(m_seekDebounceTimerId){
        killTimer(m_seekDebounceTimerId);
        m_seekDebounceTimerId = 0;
    }
    m_pendingSeekMs = -1;
    //拖动进度条也退出逐帧
    exitStepFrame();
    // // -----------
    // static int times = 0;
    // qDebug()<<"这是第"<<++times<<"次seek";
    // qint64 startNs = perfNowNs();
    // ui->video->setPerfStartNs(startNs);
    // ui->video->m_needNsDiff.store(true);
    // //-----------
    long long ms = m_durationMs*pos;
    if (m_canSeek) {
        postSeek(ms);          //内部会刷进度条 + 钉住目标 + 串行化
    } else {
        //上一次 seek 还没落地：先记下来，等 onSEEkDONE 补发。
        //不在这里硬发第二个 —— 两个 seek 同时在飞，落地顺序会乱、进度条会来回跳。
        m_pendingSeekMs = ms;
        ui->ctrlbar->setSliderValue((int)ms);
        pinSeekTarget(ms);
    }
}

void Player::timerEvent(QTimerEvent *e)
{
    //去抖定时器到点 = 用户已经停止按键 → 现在真正 seek 一次
    if (m_seekDebounceTimerId && e->timerId() == m_seekDebounceTimerId)
    {
        killTimer(m_seekDebounceTimerId);
        m_seekDebounceTimerId = 0;
        const long long target = m_pendingSeekMs;
        m_pendingSeekMs = -1;
        //用户此刻正在拖动进度条：让拖动接管，丢弃累积目标，
        //否则会在拖动过程中先跳一次（随后松手又会按拖动位置再跳一次）
        if (ui->ctrlbar->getSliderPress()) return;
        if (target >= 0){
            if (m_canSeek) {
                postSeek(target);
            } else {
                //上一次 seek 还没落地：留着，等 onSEEkDONE 再补发。
                //（以前这里直接 postMessage，既没检查 m_canSeek 也没置位，串行化在这里是漏的。）
                m_pendingSeekMs = target;
            }
        };
        return;
    }

    if (isSliderPress)return;
    if (ui->ctrlbar->getSliderPress()) return ;

}

void Player::closeEvent(QCloseEvent *e)
{
    if (m_isClosing) {          // 已经清理过了，直接放行
        e->accept();
        return;
    }
    m_isClosing = true;
    // 丢掉还没生效的 seek，避免退出过程中再触发一次
    if (m_seekDebounceTimerId) {
        killTimer(m_seekDebounceTimerId);
        m_seekDebounceTimerId = 0;
    }
    m_pendingSeekMs = -1;
    // 防用户在退出过程中乱点
    ui->ctrlbar->setDisabled(true);
    ui->topMenu->setDisabled(true);
    // 先停播放、停线程（GUI 线程执行，安全）
    op_stop(m_ctx.player);
    // 接受关闭
    e->accept();
}

void Player::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        Qt::Edges edges = hitTestEdges(e->globalPosition().toPoint());
        if (edges) {
            beginResize(edges, e->globalPosition().toPoint());
            return;                         // 消费事件，开始缩放
        }
    }
    QWidget::mousePressEvent(e);
}

void Player::mouseMoveEvent(QMouseEvent *e)
{
    if (m_isResizing) {
        updateResize(e->globalPosition().toPoint());
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void Player::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_isResizing && e->button() == Qt::LeftButton) {
        m_isResizing = false;
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

bool Player::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            Qt::Edges edges = hitTestEdges(me->globalPosition().toPoint());
            if (edges) {
                beginResize(edges, me->globalPosition().toPoint());
                return true;                // 拦截：进入缩放
            }
        }
    }
    else if (event->type() == QEvent::MouseMove && m_isResizing) {
        updateResize(static_cast<QMouseEvent*>(event)->globalPosition().toPoint());
        return true;
    }
    else if (event->type() == QEvent::MouseButtonRelease && m_isResizing) {
        m_isResizing = false;
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void Player::keyPressEvent(QKeyEvent *e)
{
    if(!m_isInit) return;
    int key = e->key();
    switch(key){
    case Qt::Key_Space:
        if(m_isPause){
            play();
        }else{
            pause();
        }
        return;
    case Qt::Key_Escape:
        if (m_isFullScreen) toggleFullScreen();
        return;
    case Qt::Key_Right:   // 快进
        ffSeekFiveSec();
        return;
    case Qt::Key_Left:    // 快退
        rewindSeekFiveSec();
        return;
    case Qt::Key_Up:      // 音量+
        adjustVolume(+0.05);
        return;
    case Qt::Key_Down:    // 音量-
        adjustVolume(-0.05);
        return;
    case Qt::Key_X:       // 降速
        changeSpeed(-0.1);
        return;
    case Qt::Key_C:       // 升速
        changeSpeed(0.1);
        return;
    case Qt::Key_F:       // 下一帧
        stepFrame(1);
        return;
    case Qt::Key_D:       // 上一帧
        stepFrame(2);
        return;
    default:
        //未处理的键交回基类（原来这句写在 switch 之后，所有分支都 return 了，是死代码）
        QWidget::keyPressEvent(e);
        return;
    }
}

void Player::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) {
        // 检查是否都是本地文件
        bool allLocal = true;
        for (const QUrl &url : e->mimeData()->urls()) {
            if (!url.isLocalFile()) {
                allLocal = false;
                break; }
        }
        if (allLocal) {
            e->acceptProposedAction();
            return;
        }
    }
    e->ignore();
}

void Player::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;
    // 如果没在播放就取第一个文件播放，其余加入列表
    if(!m_isInit){
        QString first = urls.first().toLocalFile();
        playFile(first);
    }
    for (int i = 0; i < urls.size(); ++i) {
        QString path = urls[i].toLocalFile();
        addToPlayList(path);
    }
    e->acceptProposedAction();
}





Qt::Edges Player::hitTestEdges(const QPoint &globalPos) const
{
    if (isMaximized())
        return {};                          // 最大化时不允许缩放
    QPoint p = mapFromGlobal(globalPos);
    Qt::Edges edges = {};
    if (p.x() <= kBorder)                    edges |= Qt::LeftEdge;
    else if (p.x() >= width() - kBorder)     edges |= Qt::RightEdge;
    if (p.y() <= kBorder)                    edges |= Qt::TopEdge;
    else if (p.y() >= height() - kBorder)    edges |= Qt::BottomEdge;
    return edges;
}

void Player::beginResize(Qt::Edges edges, const QPoint &globalPos)
{
    /*
     * 只有 Wayland 需要请系统接管缩放：客户端在那里不能自己改顶层窗口几何
     * （setGeometry 会被合成器忽略，表现就是"边缘拖不动"），只能请求交互式缩放。
     *
     * X11 / Windows 保持自绘缩放（和 v2.1 一致）：它们本来就允许客户端改几何，
     * 而且自绘走正常事件循环；原生缩放是模态循环，期间 Qt 不跑事件循环，
     * 拖窗口时画面/播放会停住，看起来就是卡顿。
     * 和 startSystemMove 一样，必须在鼠标按下的处理里同步调用。
     */
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        QWindow *wh = windowHandle();
        if (wh && wh->startSystemResize(edges)) {
            m_isResizing = false;       // 交给合成器，不再走自绘缩放
            return;
        }
    }

    m_isResizing  = true;
    m_resizeEdge  = edges;
    m_pressGlobal = globalPos;
    m_startGeo    = geometry();
    m_minSize     = minimumSize();
}

void Player::updateResize(const QPoint &globalPos)
{
    if (!m_isResizing)
        return;
    QPoint delta = globalPos - m_pressGlobal;
    QRect  g     = m_startGeo;
    // 按拖动的边调整几何
    if (m_resizeEdge & Qt::LeftEdge)   g.setLeft(g.left() + delta.x());
    if (m_resizeEdge & Qt::RightEdge)  g.setRight(g.right() + delta.x());
    if (m_resizeEdge & Qt::TopEdge)    g.setTop(g.top() + delta.y());
    if (m_resizeEdge & Qt::BottomEdge) g.setBottom(g.bottom() + delta.y());
    // 最小尺寸约束（setGeometry 不会自动遵守 minimumSize）
    if (m_resizeEdge & Qt::LeftEdge) {
        int maxLeft = m_startGeo.right() - m_minSize.width();
        if (g.left() > maxLeft) g.setLeft(maxLeft);
    }
    if (m_resizeEdge & Qt::TopEdge) {
        int maxTop = m_startGeo.bottom() - m_minSize.height();
        if (g.top() > maxTop) g.setTop(maxTop);
    }
    if (g.width()  < m_minSize.width())  g.setWidth(m_minSize.width());
    if (g.height() < m_minSize.height()) g.setHeight(m_minSize.height());
    setGeometry(g);
}

void Player::addToPlayList(const QString &path)
{
    ui->playList->addFile(path);
}

void Player::play()
{
    if(!m_isInit) return;
    //如果在逐帧，先退出逐帧（位置会对齐）
    exitStepFrame();
    ui->ctrlbar->stepFrameTime(false);
    ui->ctrlbar->setPausePictrue(false);
    ui->topMenu->stepFrameTime(false);
    m_stepFrame = false;
    m_isPause = false;
    op_play(m_ctx.player);
    if (m_auPlayer) m_auPlayer->setPause(false);
}

void Player::pause()
{
    if(!m_isInit) return;
    //如果在逐帧，先退出逐帧（位置会对齐）
    exitStepFrame();
    ui->ctrlbar->setPausePictrue(true);
    m_isPause = true;
    op_pause(m_ctx.player);
    if (m_auPlayer) m_auPlayer->setPause(true);
}

void Player::on_event(void *user, const op_event *ev)
{
    PlayerCtx* ctx = static_cast<PlayerCtx*>(user);
    Player* self = ctx ? ctx->self : nullptr;
    if (!self || !ev) return;

    switch (ev->type) {
    case OP_EVENT_OPENED:
        self->onEVT_PREPARED(ev);
        // 如果想打开后停在第一帧，可以在这里调 op_pause(ctx->player);
        break;

    case OP_EVENT_POSITION:
        // 这里大概每 150ms 会来一次，适合更新 UI 进度条
        self->onEVT_POSITION(ev);
        break;

    case OP_EVENT_STATE:

        break;

    case OP_EVENT_SEEK_DONE:
        self->onSEEkDONE(ev);
        break;

    case OP_EVENT_COMPLETED:
        // 播完了：有下一集就切下一集（core 不再管播放列表，由壳决定）
        self->playNext();
        break;

    case OP_EVENT_ERROR:
        self->onEVT_ERROR(ev);
        ctx->running = false;
        break;

    default:
        break;
    }
}

void Player::on_video(void *user,const op_frame *frame)
{
    PlayerCtx* ctx = static_cast<PlayerCtx*>(user);
    Player* self = ctx ? ctx->self : nullptr;
    if (!self || !frame) return;

    // 回调返回后 frame 就失效，而 GUI 线程要排队才画 → 先深拷贝一份
    op_frame* opFrame = op_frame_clone(frame);
    if (!opFrame) return;

    //抢名额失败 = GUI 线程还没消化上一帧 → 这一帧直接丢（丢帧优于无界堆积）
    if (!self->m_widget->beginPaint()) {
        op_frame_free(&opFrame);
        return;
    }

    const bool posted = QMetaObject::invokeMethod(self->m_widget, [self, opFrame]() {
        self->m_widget->setPaint(opFrame);   //接管并释放 frame（尺寸不匹配时也会释放）
        self->m_widget->endPaint();          //归还名额：无论 setPaint 走哪条分支都要还
    }, Qt::QueuedConnection);

    //投递失败（控件已销毁 / 没有事件循环）：自己收拾，否则名额会永久占死
    if (!posted) {
        op_frame_free(&opFrame);
        self->m_widget->endPaint();
    }

}

void Player::on_log(void *user, int level, const char *line)
{
    (void)user;
    const char* levels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    const char* lv = (level >= 0 && level <= 3) ? levels[level] : "DEBUG";
    qDebug()<< "[SDK " << lv << "] " << line ;
}



#include <jni.h>
#include <pthread.h>
//#include <windows.h>
#include <stdio.h>
#include <mutex>
#include <chrono>
#include <sys/stat.h>

#include "videodecoder.h"
#include "vttimeline.h"
#include "vdtaudioinfo.h"
#include "global.h"
//#include "easylogging.h"
#include "videorenderer.h"
#include "Ultils/log.h"
#include "Ultils/vtultils.h"
//#include <GL/glew.h>
//#include <GLFW/glfw3.h>
#include <unistd.h>
#ifdef __cplusplus
extern "C" {
#endif

JavaVM * g_vm;


const int CANCEL_PREVIEW = -123;
enum FLAG_START_STOP {
    F_FREE,  //Khi preview dang free
    F_RUNNING, //Dang trong qua trinh preview
    F_REQUEST_STOP //Co request stop goi xuong
};

//timeline* tl = NULL;
//std::mutex jnisync;
//std::mutex mutex_start_preview;
//std::mutex mutex_seek_preview;
//int flag_start_stop_preview = F_FREE;


timeline* preview_timeline[3] = {NULL,NULL,NULL};
std::mutex jnisync[3];
std::mutex mutex_start_preview[3];
std::mutex mutex_seek_preview[3];
VideoRenderer preview_render[3];
int flag_start_stop_preview[3] = {F_FREE,F_FREE,F_FREE};



void setPreviewConfig();
JNIEXPORT jint JNI_OnLoad(JavaVM * vm, void * reserved) {
    //g_log = new vtLog();
    LOG(INFO)<<"Start load JNI";
    g_vm = vm;
    JNIEnv * env = NULL;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) {
//        MessageBox(NULL, TEXT("Return -1"), TEXT("Load"), MB_OK);
        LOG(ERROR)<<"Load JNI fail";
        return -1;
    }
    av_register_all();
    av_log_set_level(AV_LOG_QUIET);

    setPreviewConfig();

    LOG(INFO)<<"Load JNI sucess version: "<<JNI_VERSION_1_6;

    //Tạm thời comment phần xử lý OpenGL
//    while (!glfwInit()){
//        LOG(WARNING) << "JNI: Fail to init opengl, try again";
//    }

    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_stopExport(JNIEnv * env, jobject * cls) {
    jnisync[0].lock();
    if (preview_timeline[0] != NULL) {
        preview_timeline[0]->quit();
        preview_timeline[0] = NULL;
    }
    jnisync[0].unlock();
    return 0;
}


JNIEnv *exportVideoJniEnv = NULL;
static jclass exportProgressListenerClass = NULL;
static jmethodID exportProgressListenerMethod = NULL;
jobject exportProgressListener = NULL;

void exportVideoCallback(int progress){
    if (exportProgressListener!=NULL && exportVideoJniEnv!=NULL){
        exportVideoJniEnv->CallVoidMethod(exportProgressListener, exportProgressListenerMethod, progress);
    }
}

JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_exportVideo(JNIEnv * env, jobject cls,  jstring json_conf_string, jobject callback) {
    LOG(INFO)<<"JNI Start export Video";

    jnisync[0].lock();
    if (preview_timeline[0] != NULL) {
        preview_timeline[0]->quit();
//        delete tl;
    }
    jnisync[0].unlock();

    timeline tmln;
    jnisync[0].lock();
    preview_timeline[0] = &tmln;
    jnisync[0].unlock();

    jint result = -1;
    const char * config_str;
    int frameSize = -1;
    config_str = env->GetStringUTFChars(json_conf_string, NULL);

    int err = tmln.load_video_config(config_str);
    if (err){
        jnisync[0].lock();
        preview_timeline[0] = NULL;
        jnisync[0].unlock();
        LOG(ERROR)<<"JNI Fail to start export Video";
        return err;
    }
    env->ReleaseStringUTFChars(json_conf_string, config_str);

    if(callback != NULL){
        exportVideoJniEnv = env;
        exportProgressListener = env->NewGlobalRef(callback);
        if (exportProgressListenerClass == NULL)
            exportProgressListenerClass = env->GetObjectClass(callback);
        if (exportProgressListenerMethod == NULL)
            exportProgressListenerMethod = env->GetMethodID(exportProgressListenerClass, "onExportVideoProgress", "(I)V");

        tmln.setCallback(exportVideoCallback);
    }

    result = tmln.process();

    if (result != 0){
        LOG(ERROR)<<"JNI Fail to start export Video";
    }
    else{
        LOG(INFO)<<"JNI Sucess to start export Video";
    }

//    delete timeline; timeline = NULL;
    jnisync[0].lock();
    preview_timeline[0] = NULL;
    jnisync[0].unlock();

    return result;
}

JNIEXPORT jbyteArray JNICALL Java_com_ait_videocreator_videocore_VideoCore_getVideoThumbnail(JNIEnv * env, jobject cls, jstring file, jdouble sizeRatio) {
    LOG(INFO)<<"JNI: getVideoThumbnail: start get thumbnail ";// << (file == NULL ? "NULL" : file) << "\n";
    jbyteArray ret = NULL;
    if (file == NULL)
        return ret;
    const char * filePath = env->GetStringUTFChars(file, NULL);
    //LOG(INFO)<<"JNI: get thumbnail for file: " << filePath;
    if (sizeRatio <=0) sizeRatio = 0.5; // Default is 0.5.

    //LOG(INFO)<<"JNI: get thumbnail for file: "<<filePath<<" sizeRatio: "<<sizeRatio;
    if (sizeRatio <=0) sizeRatio = 0.5; // Default is 0.5.
    //LOG(DEBUG)<<"JNI: Decoder Start to init ";
    videodecoder decoder = videodecoder();
    //LOG(DEBUG)<<"JNI: Decoder Finish to init ";
    decoder.set_width_ratio(sizeRatio);
    decoder.set_height_ratio(sizeRatio);
    //LOG(DEBUG)<<"JNI: Decoder Start to load "<<filePath;
    if (decoder.load(filePath) == 0) {
        //LOG(INFO)<<"JNI: getVideoThumnail: decoder loaded: " << filePath;
        if (decoder.process() == 0) {
            //LOG(INFO)<<"JNI: getVideoThumnail: decoder processed: " << filePath;
            int vw = decoder.get_width();
            int vh = decoder.get_height();
            int channels = decoder.get_channels();
            int matType = channels == 4 ? CV_8UC4 : CV_8UC3;

            Mat frame(Size(vw, vh), matType);
            //            decoder.seek_to(1.0);
            long time = 0;
            long thumbnail_time = ((decoder.get_duration() - 100) < 500)? (decoder.get_duration()-100):500;
            if(thumbnail_time < 0) thumbnail_time = 0;
            //LOG(DEBUG) << "Video duration =  "<<decoder.get_duration();
            //LOG(DEBUG) << "Start decode "<<filePath;
            do {
                time = decoder.get_frame(frame.data);
                if(time == -1) break;
            } while (time <= thumbnail_time);
            //LOG(INFO)<<"JNI: getVideoThumnail: decoder seeked to 0.5secs: "<<filePath;
            decoder.get_frame(frame.data);
            decoder.unload();
            //LOG(DEBUG) << "Finish decode"<<filePath;
            if (frame.data) {
                //LOG(INFO)<<"JNI: getVideoThumnail: has frame data: "<<filePath;
                int size = frame.rows * frame.cols * channels;

                if (size > 0) {
                    //LOG(INFO)<<"JNI: getVideoThumnail: frame size is valid: "<<filePath;
                    vector<uchar> buff = vector<uchar>();
                    buff.reserve(size);
                    if (imencode(channels == 4 ? ".png" : ".jpg", frame, buff)) {
                        //LOG(INFO)<<"JNI: getVideoThumnail: encoded sucessfully: "<<filePath;
                        ret = env->NewByteArray(size);
                        //cout << "JNI: getPreviewThumbnail: Array inited\n"; fflush(stdout);
                        env->SetByteArrayRegion(ret, 0, size, (jbyte*)&buff.front());
                        //cout << "JNI: getPreviewThumbnail: Array filled\n"; fflush(stdout);
                        if(ret==NULL){
                            LOG(ERROR)<<"JNI: Fail to get thumbnail"<<filePath;
                        }
                        //LOG(DEBUG)<<"JNI: SetByteArrayRegion success";
                        buff.clear();
                    }
                }
            }
            else{
                LOG(ERROR)<<"JNI: Decoder Fail to get frame"<<filePath;
            }
        }
    }
    else{
        LOG(ERROR)<<"JNI: Decoder Fail to load "<<filePath;
        env->ReleaseStringUTFChars(file, filePath);
        return ret;
    }
    LOG(DEBUG) << "JNI: Finish get thumbnail"<<filePath<<endl<<endl;
    env->ReleaseStringUTFChars(file, filePath);
    return ret;
}
static jfieldID videoInfoFile, videoInfoFps, videoInfoBitRate, videoInfoChannels, videoInfoWidth, videoInfoHeight, videoInfoDuration;

JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getVideoInfo(JNIEnv * env, jobject cls, jstring file, jobject out) {
    LOG(INFO)<<"JNI: start get Video info";
    jint ret = -1;

    if (out == NULL || file == NULL){
        LOG(ERROR)<<"JNI: Fail to get Video info";
        return -2;
    }


    const char * filePath = env->GetStringUTFChars(file, NULL);
    videodecoder decoder = videodecoder();
    decoder.set_width_ratio(1.0);
    decoder.set_height_ratio(1.0);
    if (decoder.load(filePath) == 0) {
        if (videoInfoDuration == NULL) {
            jclass videoInfoClass = env->GetObjectClass(out);
            videoInfoDuration = env->GetFieldID(videoInfoClass, "duration", "J");
            videoInfoWidth = env->GetFieldID(videoInfoClass, "width", "I");
            videoInfoHeight = env->GetFieldID(videoInfoClass, "height", "I");
            videoInfoFps = env->GetFieldID(videoInfoClass, "frameRate", "I");
            videoInfoBitRate = env->GetFieldID(videoInfoClass, "bitRate", "I");
            videoInfoChannels = env->GetFieldID(videoInfoClass, "channels", "I");
            videoInfoFile = env->GetFieldID(videoInfoClass, "file", "Ljava/lang/String;");
        }

        env->SetIntField(out, videoInfoWidth, decoder.get_width());
        env->SetIntField(out, videoInfoHeight, decoder.get_height());
        env->SetLongField(out, videoInfoDuration, decoder.get_duration());
        env->SetIntField(out, videoInfoFps, decoder.get_frameRate());
        env->SetIntField(out, videoInfoBitRate, decoder.get_bitRate());
        env->SetIntField(out, videoInfoChannels, decoder.get_channels());
        env->SetObjectField(out, videoInfoFile, file);

        ret = 0;
        decoder.unload();
    }
    env->ReleaseStringUTFChars(file, filePath);
    if (ret){
        LOG(ERROR)<<"JNI: Fail to get Video info";
    }
    return ret;
}

static jclass audioInfoClass = NULL;
static jfieldID audioInfoDuration, audioInfoSampleRate, audioInfoBitRate, audioInfoChannels, audioInfoSampleFormat = NULL;
JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getAudioInfo(JNIEnv * env, jobject cls, jstring file, jobject out) {
    LOG(INFO)<<"JNI: start get Audio info";
    jint ret = -1;

    if (out == NULL || file == NULL){
        LOG(ERROR)<<"JNI: Fail to get Video info";
        return -2;
    }

    const char * filePath = env->GetStringUTFChars(file, NULL);
    VDTAudioInfo audio_infor;
    if (audio_infor.ReadInfo(filePath) == 0) {
        if (audioInfoClass == NULL) {
            audioInfoClass = env->GetObjectClass(out);
            audioInfoDuration = env->GetFieldID(audioInfoClass, "duration", "D");
            audioInfoSampleRate = env->GetFieldID(audioInfoClass, "sampleRate", "I");
            audioInfoBitRate = env->GetFieldID(audioInfoClass, "bitRate", "I");
            audioInfoChannels = env->GetFieldID(audioInfoClass, "channels", "I");
    //        audioInfoSampleFormat = env->GetFieldID(audioInfoClass, "sampleFormat", "Ljava/lang/String;");
        }


        env->SetDoubleField(out, audioInfoDuration, audio_infor.getDuration());
        env->SetIntField(out, audioInfoSampleRate, audio_infor.getSampleRate());
        env->SetIntField(out, audioInfoBitRate, audio_infor.getBitRate());
        env->SetIntField(out, audioInfoChannels, audio_infor.getChannels());
//        char sample_fmt[50];
//        audio_infor.getSampleFormatName(sample_fmt);
//        env->SetIntField(out, audioInfoSampleFormat, sample_fmt);

        ret = 0;
    }

    env->ReleaseStringUTFChars(file, filePath);
    if(ret){
        LOG(ERROR)<<"JNI: Fail to get Video info";
    }

    return ret;
}




/* Added by TuanDA
* This functions for new previews with multithread+cache
*/

//VideoRenderer edit_board_preview;
//VideoRenderer generate_preview1;
//VideoRenderer generate_preview2;

//void setPreviewConfig()
//{
//    VideoRenderConfig edit_board_preview_config;
//    edit_board_preview_config.ID = 0;
//    edit_board_preview_config.nb_workerthreads = 1;
//    edit_board_preview_config.nb_blocks = 10;
//    edit_board_preview_config.frames_per_block = 60;
//    edit_board_preview_config.nb_cache_files = 30;
//    edit_board_preview_config.nb_frames_per_cache_file = 30;
//    strcpy(edit_board_preview_config.cache_folder, "cache/0");
//    edit_board_preview.setConfig(edit_board_preview_config);

//    VideoRenderConfig generate_preview_config;
//    generate_preview_config.nb_workerthreads = 1;
//    generate_preview_config.nb_blocks = 10;
//    generate_preview_config.frames_per_block = 30;
//    generate_preview_config.nb_cache_files = 30;
//    generate_preview_config.nb_frames_per_cache_file = 30;

//    strcpy(generate_preview_config.cache_folder, "cache/1");
//    generate_preview_config.ID = 1;
//    generate_preview1.setConfig(generate_preview_config);

//    strcpy(generate_preview_config.cache_folder, "cache/2");
//    generate_preview_config.ID = 2;
//    generate_preview2.setConfig(generate_preview_config);


//#if USING_WINDOWS_THREAD_FUNC
//    LPCWSTR path = L"cache";
//    LPCWSTR path0 = L"cache/0";
//    LPCWSTR path1 = L"cache/1";
//    LPCWSTR path2 = L"cache/2";
//    CreateDirectory(path,NULL);
//    CreateDirectory(path0,NULL);
//    CreateDirectory(path1,NULL);
//    CreateDirectory(path2,NULL);
//#else
//    mkdir("cache", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
//    mkdir("cache/0", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
//    mkdir("cache/1", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
//    mkdir("cache/2", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
//#endif

//}

//int count_preview = 0;

///*======= EDIT BOARD PREVIEW ============================*/
//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_previewVideo0(JNIEnv * env, jobject cls,  jstring json_conf_string, double startTime) {
//    LOG(INFO)<<"JNI Start preview Video at time = "<<startTime;
//    jint result = 0;
//    const char * config_str;

//    if(mutex_start_preview.try_lock()) {
//        //start preview
//        if (flag_start_stop_preview == F_RUNNING){
//            LOG(WARNING) << "JNI: Call start when running, unlock mutex_start_preview";
//            mutex_start_preview.unlock();
//            return -1;
//        }

//        /* ******************************
//        * Lock starting preview process
//        */
//        jnisync.lock();
//        std::cout<<"\n\n******************* start preview ID = "<<++count_preview<<"\n\n";
//        flag_start_stop_preview = F_RUNNING;
//        timeline tmln;
//        tl = &tmln;

//        config_str = env->GetStringUTFChars(json_conf_string, NULL);
//        chrono::milliseconds start_time = chrono::duration_cast< chrono::milliseconds >(chrono::system_clock::now().time_since_epoch());
//        result = tmln.load_video_config(config_str);
//        chrono::milliseconds end_time = chrono::duration_cast< chrono::milliseconds >(chrono::system_clock::now().time_since_epoch());
//        LOG(INFO) << "Time to load video config : "<< (end_time - start_time).count() << "ms";

//        if (result || startTime < 0.0  || startTime*1000 > tmln.get_duration() ){
//            LOG(DEBUG) << "JNI: load json fail, unlock mutex_start_preview";
//            tl = NULL;
//            env->ReleaseStringUTFChars(json_conf_string, config_str);
//            /*Unlock to exit*/
//            jnisync.unlock();
//            mutex_start_preview.unlock();
//            return -1;
//        }

//        start_time = chrono::duration_cast< chrono::milliseconds >(chrono::system_clock::now().time_since_epoch());
//        tl->mixAudio();
//        if(startTime > 0.0)
//            tl->seekAudio(startTime);
//        end_time = chrono::duration_cast< chrono::milliseconds >(chrono::system_clock::now().time_since_epoch());
//        LOG(INFO) << "Mixing Audio : "<< (end_time - start_time).count() << "ms";

//        /*Render frame and store in memory buffer and in cache file  */
//        edit_board_preview.setJSON(config_str);
//        edit_board_preview.setTimeline(tl);
//        edit_board_preview.init();
//        result = edit_board_preview.startRendering(startTime);
//        if(result != 0)
//        {
//            LOG(DEBUG) << "JNI: Preview: failed when start renderring, error = "<<result;
//            env->ReleaseStringUTFChars(json_conf_string, config_str);
//            tl = NULL;
//            jnisync.unlock();
//            mutex_start_preview.unlock();
//            return result;
//        }

//        /* ******************************
//        * starting preview successfully. Unlock here for getFrame
//        */
//        jnisync.unlock();

//        edit_board_preview.waitPlayingDone();

//        flag_start_stop_preview = F_FREE;
//        env->ReleaseStringUTFChars(json_conf_string, config_str);
//        mutex_start_preview.unlock();
//        LOG(DEBUG) << "JNI: End Preview ID = "<<count_preview;
//        return result;
//    }
//    else{
//        LOG(INFO)<<"JNI another thread is start preview";
//        return -1;
//    }
//}


//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getFrame0(JNIEnv * env, jobject cls, jbyteArray array) {
//    jint error = -1;

//    if(!edit_board_preview.isCreated()) return -1;
//    int width = 0, height = 0;
//    jnisync.lock();
//    if(!tl){
//        jnisync.unlock();
//        return -1;
//    }
//    else{
//        width = tl->get_width();
//        height = tl->get_height();
//    }
//    jnisync.unlock();

//    int frameSize  = -1;
//    if (width > 0 && height > 0 && array != NULL) {
//        error = edit_board_preview.getFrame();
//        if(error < 0)
//            return error;
//        Mat mat(Size(width, height), CV_8UC3, edit_board_preview.rendered_frame_);
//        if (frameSize == -1) {
//            long nativeSize = width * height * 3;
//            //frameSize = std::min(nativeSize, env->GetArrayLength(array)); //no matching function on MacOS
//            if(nativeSize < env->GetArrayLength(array))
//                frameSize = nativeSize;
//             else
//                frameSize =  env->GetArrayLength(array);
//        }
//        if (frameSize > 0 && array != NULL) // double check array for NULL in the case java release it.
//            env->SetByteArrayRegion(array, 0, frameSize, (jbyte*)mat.data);

//        if (env->ExceptionCheck()) {
//            LOG(ERROR)<<"JNI: Exception while getting frame!";
//            return -1;
//        }

//    }
//    return error;
//}

//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_seekVideo0(JNIEnv * env, jobject cls, jdouble time)
//{
//    LOG(INFO) <<" Seek = "<<time;

//    int error = 0;
//    if(mutex_seek_preview.try_lock()) {
//        //lock ok, start seek
//        jnisync.lock();
//        if (tl != NULL)
//        {
//            if(time > tl->get_duration())
//            {
//                jnisync.unlock();
//                mutex_seek_preview.unlock();
//                LOG(WARNING) << "JNI: seek time " << time << " over timeline duration " << tl->get_duration();
//                return -1;
//            }

//        }


//        if(edit_board_preview.isCreated() && edit_board_preview.isPlaying())
//        {
//            LOG(DEBUG) << "JNI: seek preview frame to " << time;
//            error = edit_board_preview.stopRendering(1);
//            LOG(DEBUG) << "JNI: finish stopRendering " << time<<"  error = "<<error;
//            error = edit_board_preview.startRendering(time);
//            LOG(DEBUG) << "JNI: finish startRendering " << time<<" error = "<<error;

//            if(error == 0){
//                LOG(DEBUG) << "JNI: seek audio to " << time;
//                tl->seekAudio(time);
//            }
//        }


//        jnisync.unlock();
//        mutex_seek_preview.unlock();
//        LOG(DEBUG) << "JNI: Finish seek to " << time;
//        return error;
//    }
//    else {
//        //lock fail, another thread is seek
//        LOG(WARNING) << "JNI: Seek to " << time << " failed. Another thread still seeking";
//        return -1;

//    }
//}


//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_stop0(JNIEnv * env, jobject * cls) {

//    jint result = 0;
//    LOG(DEBUG) << "JNI call stop request" << std::flush;

//    jnisync.lock();
//     std::cout<<"\n******************* stop preview ID = "<<count_preview<<"\n\n";
//    if (flag_start_stop_preview == F_FREE){
//        LOG(WARNING) << "JNI: Call stop when did not start";
//    }
//    else if (flag_start_stop_preview == F_REQUEST_STOP){
//        LOG(WARNING) << "JNI: Call multi request stop";
//    }
//    flag_start_stop_preview = F_REQUEST_STOP;
//    LOG(DEBUG) << "JNI: flag_start_stop_preview = " << flag_start_stop_preview << std::flush;

//    if (tl != NULL) {
//        tl->quit();
//        tl = NULL;
//    }
//    result = edit_board_preview.stopRendering();
//    jnisync.unlock();
//    LOG(DEBUG) << "JNI: end stop preview  with flag = " << flag_start_stop_preview <<" error = "<<result;
//    return result;
//}

//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_updateCache0(JNIEnv * env, jobject cls, jdouble starttime, jdouble endtime)
//{
//    if(edit_board_preview.isCreated())
//        edit_board_preview.update(starttime,endtime);

//    return 0;
//}

//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getProgress0(JNIEnv * env, jobject cls)
//{
//    int progress = 0;
//    if(edit_board_preview.isCreated())
//        progress = edit_board_preview.getProgress();
//    return progress;
//}


///*======= GENERATE PREVIEW 1============================*/
//timeline *generate_timeline1 = NULL;
//std::mutex lock_generate_timeline1;
//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_previewVideo1(JNIEnv * env, jobject cls,  jstring json_conf_string, double startTime) {
//    LOG(INFO)<<"\n\nJNI Start generate preview Video 1: "<<startTime;

//    timeline tmln;
//    lock_generate_timeline1.lock();
//    generate_timeline1 = &tmln;
//    lock_generate_timeline1.unlock();

//    jint result = 0;
//    const char * config_str;
//    config_str = env->GetStringUTFChars(json_conf_string, NULL);
//    result = tmln.load_video_config(config_str);
//    if (result){
//        LOG(ERROR)<<"JNI Fail to start generate preview Video 1";

//        lock_generate_timeline1.lock();
//        generate_timeline1 = NULL;
//        lock_generate_timeline1.unlock();

//        return result;
//    }
//    tmln.mixAudio();
//    if(startTime > 0.0)
//        tmln.seekAudio(startTime);

//    /*Render frame and store in memory buffer and in cache file  */
//    generate_preview1.setJSON(config_str);
//    generate_preview1.setTimeline(generate_timeline1);
//    generate_preview1.init();
//    generate_preview1.startRendering(startTime);
//    //generate_preview1.waitRenderingDone();
//    generate_preview1.waitPlayingDone();

//    env->ReleaseStringUTFChars(json_conf_string, config_str);

//    lock_generate_timeline1.lock();
//    generate_timeline1 = NULL;
//    lock_generate_timeline1.unlock();

//    cout << "JNI: End generate preview video 1.\n"<<startTime; fflush(stdout);
//    return result;
//}


//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getFrame1(JNIEnv * env, jobject cls, jbyteArray array) {
//    jint error = -1;

//    if(!generate_preview1.isCreated()) return -1;
//    lock_generate_timeline1.lock();
//    if(!generate_timeline1){
//        lock_generate_timeline1.unlock();
//        return -1;
//    }
//    lock_generate_timeline1.unlock();

//    int frameSize = -1;
//    if (generate_timeline1->get_width() > 0 && generate_timeline1->get_height() > 0 && array != NULL) {
//        error = generate_preview1.getFrame();
//        if(error < 0)
//            return error;
//        Mat mat(Size(generate_timeline1->get_width(), generate_timeline1->get_height()), CV_8UC3, generate_preview1.rendered_frame_);
//        if (frameSize == -1) {
//            long nativeSize = generate_timeline1->get_width() * generate_timeline1->get_height() * 3;
//            //frameSize = std::min(nativeSize, env->GetArrayLength(array)); //no matching function on MacOS
//            if(nativeSize < env->GetArrayLength(array))
//                frameSize = nativeSize;
//             else
//                frameSize =  env->GetArrayLength(array);
//        }
//        if (frameSize > 0 && array != NULL) // double check array for NULL in the case java release it.
//            env->SetByteArrayRegion(array, 0, frameSize, (jbyte*)mat.data);

//        if (env->ExceptionCheck()) {
//            LOG(ERROR)<<"JNI: Exception while getting frame!";
//            return -1;
//        }

//    }
//    return error;
//}

//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_seekVideo1(JNIEnv * env, jobject cls, jdouble time)
//{

//     LOG(INFO) <<" Seek 1 = "<<time<<"\n";
//     lock_generate_timeline1.lock();
//    if (generate_timeline1 != NULL) {
//        generate_timeline1->seekAudio(time);
//    }


//    if(generate_preview1.isCreated() && generate_preview1.isPlaying()){
//        generate_preview1.stopRendering(1);
//        generate_preview1.reset();
//        generate_preview1.startRendering(time);
//    }
//    lock_generate_timeline1.unlock();

//    return 0;
//}


//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_stop1(JNIEnv * env, jobject * cls) {

//    lock_generate_timeline1.lock();
//    if (generate_timeline1 != NULL) {
//        generate_timeline1->quit();
//        generate_timeline1 = NULL;
//    }

//    generate_preview1.stopRendering();
//     lock_generate_timeline1.unlock();
//    return 0;
//}

//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_updateCache1(JNIEnv * env, jobject cls, jdouble starttime, jdouble endtime)
//{
//    lock_generate_timeline1.lock();
//    if(generate_preview1.isCreated())
//        generate_preview1.update(starttime,endtime);
//    lock_generate_timeline1.unlock();
//    return 0;
//}


//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getAudioBuffer1(JNIEnv * env, jobject cls, jbyteArray bufferByte, jint size)
//{
//    if (bufferByte == NULL || size <= 0 || generate_timeline1 == NULL)
//        return -1;

//    char * audio_buf;
//    jint res;
//    audio_buf = (char*)env->GetPrimitiveArrayCritical(bufferByte, NULL);

//    res = generate_timeline1->get_audio(audio_buf, size);

//    if (res < 0){
//        LOG(ERROR)<<"JNI: Exception while getting audioBuffer!";
//    }
//    env->ReleasePrimitiveArrayCritical(bufferByte, audio_buf, 0);
//    return res;
//}


//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getProgress1(JNIEnv * env, jobject cls)
//{
//    int progress = 0;
//    if(generate_preview1.isCreated())
//        progress = generate_preview1.getProgress();
//    return progress;
//}

///*======= GENERATE PREVIEW 2 ============================*/
//timeline *generate_timeline2 = NULL;
//std::mutex lock_generate_timeline2;
//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_previewVideo2(JNIEnv * env, jobject cls,  jstring json_conf_string, double startTime) {
//    LOG(INFO)<<"\n\nJNI Start generate preview Video 2: "<<startTime;

//    timeline tmln;
//    lock_generate_timeline2.lock();
//    generate_timeline2 = &tmln;
//    lock_generate_timeline2.unlock();


//    jint result = 0;
//    const char * config_str;
//    config_str = env->GetStringUTFChars(json_conf_string, NULL);
//    result = tmln.load_video_config(config_str);
//    if (result){
//        LOG(ERROR)<<"JNI Fail to start generate preview Video 2";
//        lock_generate_timeline2.lock();
//        generate_timeline2 = NULL;
//        lock_generate_timeline2.unlock();
//        return result;
//    }

//    tmln.mixAudio();
//    if(startTime > 0.0)
//        tmln.seekAudio(startTime);

//    /*Render frame and store in memory buffer and in cache file  */
//    generate_preview2.setJSON(config_str);
//    generate_preview2.setTimeline(generate_timeline2);
//    generate_preview2.init();
//    generate_preview2.startRendering(startTime);
//    //generate_preview2.waitRenderingDone();
//    generate_preview2.waitPlayingDone();
//    env->ReleaseStringUTFChars(json_conf_string, config_str);
//    lock_generate_timeline2.lock();
//    generate_timeline2 = NULL;
//    lock_generate_timeline2.unlock();

//    cout << "JNI: End generate preview video 2.\n"<<startTime; fflush(stdout);
//    return result;
//}


//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getFrame2(JNIEnv * env, jobject cls, jbyteArray array) {
//    jint error = -1;

//    if(!generate_preview2.isCreated()) return -1;
//    lock_generate_timeline2.lock();
//    if(!generate_timeline2){
//        lock_generate_timeline2.unlock();
//        return -1;
//    }
//    lock_generate_timeline2.unlock();

//    int frameSize = -1;
//    if (generate_timeline2->get_width() > 0 && generate_timeline2->get_height() > 0 && array != NULL) {
//        error = generate_preview2.getFrame();
//        if(error < 0)
//            return error;
//        Mat mat(Size(generate_timeline2->get_width(), generate_timeline2->get_height()), CV_8UC3, generate_preview2.rendered_frame_);
//        if (frameSize == -1) {
//            long nativeSize = generate_timeline2->get_width() * generate_timeline2->get_height() * 3;
//            //frameSize = std::min(nativeSize, env->GetArrayLength(array)); //no matching function on MacOS
//            if(nativeSize < env->GetArrayLength(array))
//                frameSize = nativeSize;
//             else
//                frameSize =  env->GetArrayLength(array);
//        }
//        if (frameSize > 0 && array != NULL) // double check array for NULL in the case java release it.
//            env->SetByteArrayRegion(array, 0, frameSize, (jbyte*)mat.data);

//        if (env->ExceptionCheck()) {
//            LOG(ERROR)<<"JNI: Exception while getting frame!";
//            return -1;
//        }

//    }
//    return error;
//}

//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_seekVideo2(JNIEnv * env, jobject cls, jdouble time)
//{
//    LOG(INFO) <<"\Seek 2 = "<<time<<"\n";
//    lock_generate_timeline2.lock();
//    if (generate_timeline2 != NULL) {
//        generate_timeline2->seekAudio(time);
//    }

//    if(generate_preview2.isCreated() && generate_preview2.isPlaying())
//    {
//        generate_preview2.stopRendering(1);
//        generate_preview2.reset();
//        generate_preview2.startRendering(time);
//    }
//    lock_generate_timeline2.unlock();
//    return 0;
//}


//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_stop2(JNIEnv * env, jobject * cls) {

//    lock_generate_timeline2.lock();
//    if (generate_timeline2 != NULL) {
//        generate_timeline2->quit();
//        generate_timeline2 = NULL;
//    }

//    generate_preview2.stopRendering();
//    lock_generate_timeline2.unlock();
//    return 0;
//}

//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_updateCache2(JNIEnv * env, jobject cls, jdouble starttime, jdouble endtime)
//{
//    lock_generate_timeline2.lock();
//    if(generate_preview2.isCreated())
//        generate_preview2.update(starttime,endtime);
//    lock_generate_timeline2.unlock();
//    return 0;
//}


//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getAudioBuffer2(JNIEnv * env, jobject cls, jbyteArray bufferByte, jint size)
//{
//    if (bufferByte == NULL || size <= 0 || generate_timeline2 == NULL)
//        return -1;

//    char * audio_buf;
//    jint res;
//    audio_buf = (char*)env->GetPrimitiveArrayCritical(bufferByte, NULL);

//    res = generate_timeline2->get_audio(audio_buf, size);

//    if (res < 0){
//        LOG(ERROR)<<"JNI: Exception while getting audioBuffer!";
//    }
//    env->ReleasePrimitiveArrayCritical(bufferByte, audio_buf, 0);
//    return res;
//}

//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getProgress2(JNIEnv * env, jobject cls)
//{
//    int progress = 0;
//    if(generate_preview2.isCreated())
//        progress = generate_preview2.getProgress();
//    return progress;
//}
/*
 * TUANDA
    END NEW JNI INTERFACE FOR PREVIEW WITH MULTITHREAD + CACHE
*/



/*TUANDA
* MERGE 3 PREVIEW
*
*/
/*======= EDIT BOARD PREVIEW ============================*/

void setPreviewConfig()
{
    VideoRenderConfig edit_board_preview_config;
    edit_board_preview_config.ID = 0;
    edit_board_preview_config.nb_workerthreads = 1;
    edit_board_preview_config.nb_blocks = 10;
    edit_board_preview_config.frames_per_block = 60;
    edit_board_preview_config.nb_cache_files = 30;
    edit_board_preview_config.nb_frames_per_cache_file = 30;
    strcpy(edit_board_preview_config.cache_folder, "cache/0");
    preview_render[0].setConfig(edit_board_preview_config);

    VideoRenderConfig generate_preview_config;
    generate_preview_config.nb_workerthreads = 1;
    generate_preview_config.nb_blocks = 10;
    generate_preview_config.frames_per_block = 30;
    generate_preview_config.nb_cache_files = 30;
    generate_preview_config.nb_frames_per_cache_file = 30;

    strcpy(generate_preview_config.cache_folder, "cache/1");
    generate_preview_config.ID = 1;
    preview_render[1].setConfig(generate_preview_config);

    strcpy(generate_preview_config.cache_folder, "cache/2");
    generate_preview_config.ID = 2;
    preview_render[2].setConfig(generate_preview_config);


#if USING_WINDOWS_THREAD_FUNC
    LPCWSTR path = L"cache";
    LPCWSTR path0 = L"cache/0";
    LPCWSTR path1 = L"cache/1";
    LPCWSTR path2 = L"cache/2";
    CreateDirectory(path,NULL);
    CreateDirectory(path0,NULL);
    CreateDirectory(path1,NULL);
    CreateDirectory(path2,NULL);
#else
    mkdir("cache", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    mkdir("cache/0", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    mkdir("cache/1", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    mkdir("cache/2", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
#endif
}



JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_previewVideo(JNIEnv * env, jobject cls, jint previewID,  jstring json_conf_string, double startTime) {
    LOG(INFO)<<"JNI Start preview ID = "<<previewID<<" at time = "<<startTime;
    jint result = 0;
    const char * config_str;

    if(mutex_start_preview[previewID].try_lock()) {
        //start preview
        if (flag_start_stop_preview[previewID] == F_RUNNING){
            LOG(WARNING) << "JNI: ID "<<previewID<<" Call start when running, unlock mutex_start_preview";
            mutex_start_preview[previewID].unlock();
            return -1;
        }

        /* ******************************
        * Lock starting preview process
        */
        jnisync[previewID].lock();
        flag_start_stop_preview[previewID] = F_RUNNING;
        timeline tmln;
        preview_timeline[previewID] = &tmln;

        config_str = env->GetStringUTFChars(json_conf_string, NULL);
        chrono::milliseconds start_time = chrono::duration_cast< chrono::milliseconds >(chrono::system_clock::now().time_since_epoch());
        result = tmln.load_video_config(config_str);
        chrono::milliseconds end_time = chrono::duration_cast< chrono::milliseconds >(chrono::system_clock::now().time_since_epoch());
        LOG(INFO) << "JNI: ID "<<previewID<<" Time to load video config : "<< (end_time - start_time).count() << "ms";

        if (result || startTime < 0.0  || (int)(startTime*1000) > tmln.get_duration() ){
            LOG(DEBUG) << "JNI: ID "<<previewID<<" load json fail, unlock mutex_start_preview duration = "<<tmln.get_duration()<<" start = "<<(int)(startTime*1000) ;
            preview_timeline[previewID] = NULL;
            env->ReleaseStringUTFChars(json_conf_string, config_str);
            /*Unlock to exit*/
            jnisync[previewID].unlock();
            mutex_start_preview[previewID].unlock();
            return -1;
        }

        start_time = chrono::duration_cast< chrono::milliseconds >(chrono::system_clock::now().time_since_epoch());
        tmln.mixAudio();
        if(startTime > 0.0)
            tmln.seekAudio(startTime);
        end_time = chrono::duration_cast< chrono::milliseconds >(chrono::system_clock::now().time_since_epoch());
        LOG(INFO) << "JNI: ID "<<previewID<<" Mixing Audio : "<< (end_time - start_time).count() << "ms";

        /*Render frame and store in memory buffer and in cache file  */
        preview_render[previewID].setJSON(config_str);
        preview_render[previewID].setTimeline(preview_timeline[previewID]);
        preview_render[previewID].init();
        result = preview_render[previewID].startRendering(startTime);
        if(result != 0)
        {
            LOG(DEBUG) << "JNI: ID "<<previewID<<"  failed when start renderring, error = "<<result;
            env->ReleaseStringUTFChars(json_conf_string, config_str);
            preview_timeline[previewID] = NULL;
            jnisync[previewID].unlock();
            mutex_start_preview[previewID].unlock();
            return result;
        }

        /* ******************************
        * starting preview successfully. Unlock here for getFrame
        */
        jnisync[previewID].unlock();

        preview_render[previewID].waitPlayingDone();

        flag_start_stop_preview[previewID] = F_FREE;
        env->ReleaseStringUTFChars(json_conf_string, config_str);
        mutex_start_preview[previewID].unlock();
        return result;
    }
    else{
        LOG(INFO)<<"JNI: ID "<<previewID<<" another thread is start preview";
        return -1;
    }
}


JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getFrame(JNIEnv * env, jobject cls, jint previewID, jbyteArray array) {
    jint error = -1;
    if(!preview_render[previewID].isCreated())
        return -1;

    jnisync[previewID].lock();
    if(!preview_timeline[previewID]){
        jnisync[previewID].unlock();
        return -1;
    }
    jnisync[previewID].unlock();

    int frameSize  = -1;
    int width = preview_timeline[previewID]->get_width();
    int height = preview_timeline[previewID]->get_height();
    if (width > 0 && height > 0 && array != NULL) {
        error = preview_render[previewID].getFrame();
        if(error < 0)
            return error;
        Mat mat(Size(width, height), CV_8UC3, preview_render[previewID].rendered_frame_);
        if (frameSize == -1) {
            long nativeSize = width * height * 3;
            //frameSize = std::min(nativeSize, env->GetArrayLength(array)); //no matching function on MacOS
            if(nativeSize < env->GetArrayLength(array))
                frameSize = nativeSize;
             else
                frameSize =  env->GetArrayLength(array);
        }
        if (frameSize > 0 && array != NULL) // double check array for NULL in the case java release it.
            env->SetByteArrayRegion(array, 0, frameSize, (jbyte*)mat.data);

        if (env->ExceptionCheck()) {
            LOG(ERROR)<<"JNI: ID "<<previewID<<" Exception while getting frame!";
            return -1;
        }
    }
    return error;
}

JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getAudioBuffer(JNIEnv * env, jobject cls, jint previewID, jbyteArray bufferByte, jint size)
{
    if (bufferByte == NULL || size <= 0 || preview_timeline[previewID] == NULL)
        return -1;

    char * audio_buf;
    jint res;
    audio_buf = (char*)env->GetPrimitiveArrayCritical(bufferByte, NULL);

    jnisync[previewID].lock();
    res = preview_timeline[previewID]->get_audio(audio_buf, size);
    jnisync[previewID].unlock();

    if (res < 0){
        LOG(ERROR)<<"JNI: ID "<<previewID<<" Exception while getting audioBuffer!";
    }
    env->ReleasePrimitiveArrayCritical(bufferByte, audio_buf, 0);
    return res;
}

JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_seekVideo(JNIEnv * env, jobject cls,jint previewID, jdouble time)
{
    LOG(INFO) <<"JNI: ID "<<previewID<<" Seek = "<<time;

    int error = 0;
    if(mutex_seek_preview[previewID].try_lock()) {
        //lock ok, start seek
        jnisync[previewID].lock();
        if (preview_timeline[previewID] != NULL)
        {
            if(time > preview_timeline[previewID]->get_duration())
            {
                jnisync[previewID].unlock();
                mutex_seek_preview[previewID].unlock();
                LOG(WARNING) << "JNI: ID "<<previewID<<" seek time " << time << " over timeline duration " << preview_timeline[previewID]->get_duration();
                return -1;
            }

        }

        if(preview_render[previewID].isCreated() && preview_render[previewID].isPlaying())
        {
            preview_render[previewID].stopRendering(1);
            error = preview_render[previewID].startRendering(time);

            if(error == 0){
                LOG(DEBUG) << "JNI: ID "<<previewID<<" seek audio to " << time;
                preview_timeline[previewID]->seekAudio(time);
            }else{
                LOG(DEBUG) << "JNI: ID "<<previewID<<" failed to start renderring at "<<time<<" error = "<<error;
            }
        }

        jnisync[previewID].unlock();
        mutex_seek_preview[previewID].unlock();
        LOG(DEBUG) << "JNI: ID "<<previewID<<" Finish seek to " << time;
        return error;
    }
    else {
        //lock fail, another thread is seek
        LOG(WARNING) << "JNI: ID "<<previewID<<" Seek to " << time << " failed. Another thread still seeking";
        return -1;

    }
}


JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_stop(JNIEnv * env, jobject * cls, jint previewID) {

    jint result = 0;
    LOG(DEBUG) << "JNI: ID "<<previewID<<" call stop request" << std::flush;

    jnisync[previewID].lock();
    if (flag_start_stop_preview[previewID] == F_FREE){
        LOG(WARNING) << "JNI: ID "<<previewID<<" Call stop when did not start";
    }
    else if (flag_start_stop_preview[previewID] == F_REQUEST_STOP){
        LOG(WARNING) << "JNI: ID "<<previewID<<" Call multi request stop";
    }
    flag_start_stop_preview[previewID] = F_REQUEST_STOP;

    if (preview_timeline[previewID] != NULL) {
         preview_timeline[previewID] = NULL;
    }

    result = preview_render[previewID].stopRendering();
    jnisync[previewID].unlock();
    LOG(DEBUG) << "JNI: ID "<<previewID<<" end stop preview  with flag = " << flag_start_stop_preview[previewID] <<" error = "<<result;
    return result;
}

JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_updateCache(JNIEnv * env, jobject cls,jint previewID,  jdouble starttime, jdouble endtime)
{
    if(preview_render[previewID].isCreated())
        preview_render[previewID].update(starttime,endtime);

    return 0;
}

JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getProgress(JNIEnv * env, jobject cls, jint previewID)
{
    int progress = 0;
    if(preview_render[previewID].isCreated())
        progress = preview_render[previewID].getProgress();
    return progress;
}



//JNIEXPORT jbyteArray JNICALL Java_com_ait_videocreator_videocore_VideoCore_snapshot(JNIEnv * env, jobject cls,  jstring json_conf_string, jdouble time, jint snapIndex, jint cutIndex){
////JNIEXPORT jbyteArray JNICALL Java_com_ait_videocreator_videocore_VideoCore_snapshot(JNIEnv * env, jobject cls,  jstring json_conf_string, jdouble time) {
//    LOG(INFO)<<"JNI snapshot";
//    jbyteArray ret = NULL;
//    timeline tmln;

//    const char * config_str;
//    config_str = env->GetStringUTFChars(json_conf_string, NULL);
//    int err = tmln.load_video_config(config_str);
//    if (err){
//        LOG(ERROR)<<"JNI Fail to load json";
//        env->ReleaseStringUTFChars(json_conf_string, config_str);
//        return ret;
//    }

//    Mat frame(Size(tmln.get_width(), tmln.get_height()), CV_8UC4 );
//    tmln.snapshot(frame, time, -1, -1);

//    /*encode to png*/
//    if (frame.data) {
//       int size = frame.rows * frame.cols * 4;
//       if (size > 0) {
//           vector<uchar> buff = vector<uchar>();
//           buff.reserve(size);
//           if (imencode(".png", frame, buff)) {
//               ret = env->NewByteArray(size);
//               env->SetByteArrayRegion(ret, 0, size, (jbyte*)&buff.front());
//               if(ret==NULL){
//                   LOG(ERROR)<<"JNI: Fail to render snapshot";
//                   env->ReleaseStringUTFChars(json_conf_string, config_str);
//                   return NULL;
//               }
//               LOG(DEBUG)<<"JNI: create snapshot successfullly";
//               buff.clear();
//           }
//       }
//   }

//    env->ReleaseStringUTFChars(json_conf_string, config_str);
//    cout << "JNI: End get snapshot.\n"; fflush(stdout);
//    return ret;
//}


JNIEXPORT jbyteArray JNICALL Java_com_ait_videocreator_videocore_VideoCore_snapshot(JNIEnv * env, jobject cls,  jint previewID, jstring json_conf_string, jdouble time, jint snapIndex, jint cutIndex){
    LOG(INFO)<<"JNI: previewID = "<<previewID<< " snapshot  at time = "<<time;

    int ret = 0;
    jbyteArray result = NULL;
    timeline *tmln = NULL;
    VideoRenderer *preview = &preview_render[previewID];
    const char * config_str;
    long nativeSize = 0;
    cv::Mat *frame = NULL;
    int frameSize;
    std::string filename;

    config_str = env->GetStringUTFChars(json_conf_string, NULL);

    if(!preview){
        goto cleanup;
    }

    if(preview->isCreated())
    {
        frame = new Mat(Size(preview->getFrameWidth(), preview->getFrameHeight()), CV_8UC3 );
        if(snapIndex < 0)
        {
            /*When call snapshot in timeline of generate, if preview is cached, get snapshot from cache*/
            ret = preview->getSnapshot((char*)frame->data,time);
            if(ret >= 0){
                LOG(INFO)<<"JNI: previewID = "<<previewID<<" Got snapshot from cache\n";
                nativeSize = preview->getFrameWidth()* preview->getFrameHeight()*3;
                goto process_frame;
            }
        }

        /*If call snapshot when playing, don't need to parse JSON, clone current timeline*/
        if(preview->isPlaying()){
            LOG(INFO)<<"JNI: previewID = "<<previewID<<" Got snapshot by clone main timeline\n";
            tmln = new timeline(*preview->main_timeline_);
            tmln->snapshot(*frame, time, snapIndex, cutIndex);
            nativeSize = tmln->get_width() * tmln->get_height() * 3;
            goto process_frame;
        }
    }

    /*If call snapshot when select part in generate
     * compare json with json already been saved in previous action
     * if the json is the same, don't need to parse json, clone timeline of previous action
    */
    if(preview->compareJson(config_str)){
        if(preview->getTimeline())
        {
            LOG(INFO)<<"JNI: previewID = "<<previewID<<" Got snapshot by clone pre timeline";
            tmln = new timeline(*preview->getTimeline());
            frame = new Mat(Size(tmln->get_width(), tmln->get_height()), CV_8UC3 );
            tmln->snapshot(*frame, time, snapIndex, cutIndex);
            nativeSize = tmln->get_width() * tmln->get_height() * 3;
            goto process_frame;
        }
    }

    /*If this is the first time to call snapshot(no cache, no previous action information or something else
     * Create new timeline, load json
     * save this json, and timeline for the next call
    */
    LOG(INFO)<<"JNI: previewID = "<<previewID<<" Got snapshot from load new json";
    tmln = new timeline();
    ret = tmln->load_video_config(config_str);
    if (ret){
        LOG(ERROR)<<"JNI Fail to load json\n";
        goto cleanup;
    }

    if(frame) delete frame;
    frame = new Mat(Size(tmln->get_width(), tmln->get_height()), CV_8UC3 );
    tmln->snapshot(*frame, time, snapIndex, cutIndex);
    nativeSize = tmln->get_width() * tmln->get_height() * 3;

    /*save this json for the next call*/
    preview->setJSON(config_str);
    preview->savePreTimeline(tmln);

process_frame:

    /*encode to png*/
    if (frame->data) {
       int size = frame->rows * frame->cols * 4;
       if (size > 0) {
           vector<uchar> buff = vector<uchar>();
           buff.reserve(size);
           if (imencode(".png", *frame, buff)) {
               result = env->NewByteArray(size);
               env->SetByteArrayRegion(result, 0, size, (jbyte*)&buff.front());
               if(result == NULL){
                   LOG(ERROR)<<"JNI: Fail to render snapshot";
                   goto cleanup;
               }
               LOG(DEBUG)<<"JNI: create snapshot successfullly";
               buff.clear();
           }
       }
   }

//    if(frame){
//        std::string filename = std::to_string(previewID) + ".jpg";
//        imwrite(filename, *frame);
//    }

 cleanup:
    env->ReleaseStringUTFChars(json_conf_string, config_str);
    if(tmln) delete tmln;
    if(frame) delete frame;
    LOG(INFO)<<"JNI: previewID = "<<previewID << " End get snapshot\n";
    return result;
}

JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_snapshot2buffer(JNIEnv * env, jobject cls,  jint previewID, jstring json_conf_string, jdouble time, jbyteArray array, jint snapIndex, jint cutIndex){
//JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_snapshot2buffer(JNIEnv * env, jobject cls,  jstring json_conf_string, jdouble time, jbyteArray array) {
    LOG(INFO)<<"JNI: previewID = "<<previewID<< " snapshot to buffer  at time = "<<time;

    int ret = 0;
    timeline *tmln = NULL;
    VideoRenderer *preview = &preview_render[previewID];
    const char * config_str;
    long nativeSize = 0;
    cv::Mat *frame = NULL;
    int frameSize;
    std::string filename;

    config_str = env->GetStringUTFChars(json_conf_string, NULL);

    if(!preview){
        goto cleanup;
    }

    if(preview->isCreated())
    {
        frame = new Mat(Size(preview->getFrameWidth(), preview->getFrameHeight()), CV_8UC3 );        
        if(snapIndex < 0)
        {
            /*When call snapshot in timeline of generate, if preview is cached, get snapshot from cache*/
            ret = preview->getSnapshot((char*)frame->data,time);
            if(ret >= 0){
                LOG(INFO)<<"JNI: previewID = "<<previewID<<" Got snapshot from cache\n";
                nativeSize = preview->getFrameWidth()* preview->getFrameHeight()*3;
                goto process_frame;
            }
        }

        /*If call snapshot when playing, don't need to parse JSON, clone current timeline*/
        if(preview->isPlaying()){
            LOG(INFO)<<"JNI: previewID = "<<previewID<<" Got snapshot by clone main timeline\n";
            tmln = new timeline(*preview->main_timeline_);
            tmln->snapshot(*frame, time, snapIndex, cutIndex);
            nativeSize = tmln->get_width() * tmln->get_height() * 3;
            goto process_frame;
        }        
    }

    /*If call snapshot when select part in generate
     * compare json with json already been saved in previous action
     * if the json is the same, don't need to parse json, clone timeline of previous action
    */
    if(preview->compareJson(config_str)){
        if(preview->getTimeline())
        {
            LOG(INFO)<<"JNI: previewID = "<<previewID<<" Got snapshot by clone pre timeline";
            tmln = new timeline(*preview->getTimeline());
            frame = new Mat(Size(tmln->get_width(), tmln->get_height()), CV_8UC3 );
            tmln->snapshot(*frame, time, snapIndex, cutIndex);
            nativeSize = tmln->get_width() * tmln->get_height() * 3;
            goto process_frame;
        }
    }

    /*If this is the first time to call snapshot(no cache, no previous action information or something else
     * Create new timeline, load json
     * save this json, and timeline for the next call
    */
    LOG(INFO)<<"JNI: previewID = "<<previewID<<" Got snapshot from load new json";
    tmln = new timeline();
    ret = tmln->load_video_config(config_str);
    if (ret){
        LOG(ERROR)<<"JNI Fail to load json\n";
        goto cleanup;
    }

    if(frame) delete frame;
    frame = new Mat(Size(tmln->get_width(), tmln->get_height()), CV_8UC3 );
    tmln->snapshot(*frame, time, snapIndex, cutIndex);
    nativeSize = tmln->get_width() * tmln->get_height() * 3;

    /*save this json for the next call*/
    preview->setJSON(config_str);
    preview->savePreTimeline(tmln);

process_frame:

   // int frameSize = std::min(nativeSize, env->GetArrayLength(array));
    if(nativeSize < env->GetArrayLength(array))
        frameSize = nativeSize;
     else
        frameSize =  env->GetArrayLength(array);
    if (frameSize > 0 && array != NULL && frame) // double check array for NULL in the case java release it.
        env->SetByteArrayRegion(array, 0, frameSize, (jbyte*)frame->data);

    if (env->ExceptionCheck()) {
        LOG(ERROR)<<"JNI: Exception while getting snapshot!";
        goto cleanup;
    }

//    if(frame){
//        std::string filename = std::to_string(previewID) + ".jpg";
//        imwrite(filename, *frame);
//    }

 cleanup:
    env->ReleaseStringUTFChars(json_conf_string, config_str);
    if(tmln) delete tmln;
    if(frame) delete frame;
    LOG(INFO)<<"JNI: previewID = "<<previewID << " End get snapshot 2 buffer.\n";
    return ret;
}

JNIEXPORT jint JNICALL Java_com_ait_videocreator_videocore_VideoCore_getGifDuration(JNIEnv * env, jobject cls, jstring file){
    LOG(INFO)<<"JNI: start get Gif image duration";
    jint ret = -1;
    if (file == NULL){
        LOG(ERROR)<<"JNI: Fail to get Gif image duration";
        return -1;
    }
    const char * filePath = env->GetStringUTFChars(file, NULL);
    ret = vtultils::vtGetGifImageDuration(filePath);
    LOG(INFO)<<"JNI: end get Gif duration: "<<ret<<"ms";
    env->ReleaseStringUTFChars(file, filePath);
    return ret;
}
JNIEXPORT jbyteArray JNICALL Java_com_ait_videocreator_videocore_VideoCore_getImageFromAss(JNIEnv * env, jobject cls, jstring string_ass, jint preview_width, jint preview_height){
    jbyteArray result = NULL;
    const char* c_ass_str;
    c_ass_str = env->GetStringUTFChars(string_ass, NULL);
    LOG(INFO)<<"JNI: Start get image from ass file = "<<c_ass_str << " width = "<< preview_width << " height = " << preview_height;
    vtsubtitle *v = new vtsubtitle();
    //int err = v->load(file_ass_str, preview_width, preview_height, "");
    int err = v->loadBuffer(c_ass_str, preview_width, preview_height, "");
    if (err != 0){
        LOG(ERROR)<<"JNI: Fail to get image from ass file";
        env->ReleaseStringUTFChars(string_ass, c_ass_str);
        delete v;
        return result;
    }
    Mat frame = v->get4chanImage();
    if (frame.data) {
       int size = frame.rows * frame.cols * 4;
       if (size > 0) {
           vector<uchar> buff = vector<uchar>();
           buff.reserve(size);
           if (imencode(".png", frame, buff)) {
               result = env->NewByteArray(size);
               env->SetByteArrayRegion(result, 0, size, (jbyte*)&buff.front());
               if(result == NULL){
                   LOG(ERROR)<<"JNI: Fail to get image from ass";
               }
               LOG(DEBUG)<<"JNI: get image from ass successfullly width = "<< frame.cols << "height = " << frame.rows;
               buff.clear();
           }
       }
   }
    else{
        LOG(ERROR)<<"JNI: Fail to get image from ass";
    }
   env->ReleaseStringUTFChars(string_ass, c_ass_str);
   delete v;
   LOG(INFO)<<"JNI: Finish to get image from ass return "<< result;
   fflush(stdout);
   return result;
}

#ifdef __cplusplus
}
#endif

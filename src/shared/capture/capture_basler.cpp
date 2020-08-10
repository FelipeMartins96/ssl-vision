/*
 * capture_basler.cpp
 *
 *  Created on: Nov 21, 2016
 *      Author: root
 */

#include "capture_basler.h"

#include <vector>
#include <string>

#define MUTEX_LOCK mutex.lock()
#define MUTEX_UNLOCK mutex.unlock()

int BaslerInitManager::count = 0;

void BaslerInitManager::register_capture() {
	if (count++ == 0) {
		Pylon::PylonInitialize();
	}
}

void BaslerInitManager::unregister_capture() {
	if (--count == 0) {
		Pylon::PylonTerminate();
	}
}

CaptureBasler::CaptureBasler(VarList* _settings, QObject* parent) :
		QObject(parent), CaptureInterface(_settings) {
	is_capturing = false;
	camera = NULL;
	ignore_capture_failure = false;
	converter.OutputPixelFormat = Pylon::PixelType_RGB8packed;
	//camera.PixelFormat.SetValue(Basler_GigECamera::PixelFormat_YUV422Packed, true);
	last_buf = NULL;

	settings->addChild(vars = new VarList("Capture Settings"));
	settings->removeFlags(VARTYPE_FLAG_HIDE_CHILDREN);
	vars->removeFlags(VARTYPE_FLAG_HIDE_CHILDREN);
	v_color_mode = new VarStringEnum("color mode",
			Colors::colorFormatToString(COLOR_RGB8));
	v_color_mode->addItem(Colors::colorFormatToString(COLOR_YUV422_UYVY));
	v_color_mode->addItem(Colors::colorFormatToString(COLOR_RGB8));
	vars->addChild(v_color_mode);

	vars->addChild(v_camera_id = new VarInt("Camera ID", 0, 0, 3));

	v_balance_ratio_red = new VarInt("Balance Ratio Red", 64, 0, 255);
	vars->addChild(v_balance_ratio_red);

	v_balance_ratio_green = new VarInt("Balance Ratio Green", 64, 0, 255);
	vars->addChild(v_balance_ratio_green);

	v_balance_ratio_blue = new VarInt("Balance Ratio Blue", 64, 0, 255);
	vars->addChild(v_balance_ratio_blue);

	v_auto_gain = new VarBool("auto gain", true);
	vars->addChild(v_auto_gain);

	v_gain = new VarDouble("gain", 300, 200, 1000);
	vars->addChild(v_gain);

	v_gamma_enable = new VarBool("enable gamma correction", false);
	vars->addChild(v_gamma_enable);

	v_gamma = new VarDouble("gamma", 0.5, 0, 1);
	vars->addChild(v_gamma);

	v_black_level = new VarDouble("black level", 64, 0, 1000);
	vars->addChild(v_black_level);

	v_auto_exposure = new VarBool("auto exposure", false);
	vars->addChild(v_auto_exposure);

	v_manual_exposure = new VarDouble("manual exposure (μs)", 10000, 1000,
			30000);
	vars->addChild(v_manual_exposure);

	current_id = 0;

	mvc_connect(settings);
	mvc_connect(vars);
}

CaptureBasler::~CaptureBasler() {
	vars->deleteAllChildren();
}

bool CaptureBasler::_buildCamera() {
	BaslerInitManager::register_capture();
	Pylon::DeviceInfoList devices;
	int amt = Pylon::CTlFactory::GetInstance().EnumerateDevices(devices);
	current_id = v_camera_id->get();
    printf("Current camera id: %d\n", current_id);
	if (amt > current_id) {
		Pylon::CDeviceInfo info = devices[current_id];

		camera = new Pylon::CBaslerGigEInstantCamera(
				Pylon::CTlFactory::GetInstance().CreateDevice(info));
        printf("Opening camera %d...\n", current_id);
		camera->Open();
        printf("Setting interpacket delay...\n");
		camera->GevSCPD.SetValue(600); //TODO: check effect of changing this
		if(GenApi::IsWritable(camera->ChunkModeActive)){
            camera->ChunkModeActive.SetValue(true);
            camera->ChunkSelector.SetValue(Basler_GigECamera::ChunkSelector_Timestamp);
            camera->ChunkEnable.SetValue(true);
            camera->ChunkSelector.SetValue(Basler_GigECamera::ChunkSelector_Framecounter);
            camera->ChunkEnable.SetValue(true);
        }else{
		    return false; //Camera does not support accurate timings
		}
        printf("Done!\n");
		is_capturing = true;
		return true;
	}
	return false;
}

bool CaptureBasler::startCapture() {
	MUTEX_LOCK;
	try {
		if (camera == NULL) {
			if (!_buildCamera()) {
                // Did not make a camera!
                return false;
            }
		}
		camera->StartGrabbing(Pylon::GrabStrategy_LatestImageOnly);
	} catch (Pylon::GenericException& e) {
        printf("Pylon exception: %s", e.what());
        delete camera;
        camera = NULL;
        current_id = -1;
		MUTEX_UNLOCK;
        return false;
	} catch (...) {
        printf("Uncaught exception at line 132\n");
		MUTEX_UNLOCK;
		throw;
	}
	MUTEX_UNLOCK;
	return true;
}

bool CaptureBasler::_stopCapture() {
	if (is_capturing) {
	    camera->ChunkModeActive.SetValue(false);
		camera->StopGrabbing();
		camera->Close();
		is_capturing = false;
		return true;
	}
	return false;
}

bool CaptureBasler::stopCapture() {
	MUTEX_LOCK;
	bool stopped;
	try {
		stopped = _stopCapture();
		if (stopped) {
			delete camera;
			camera = 0;
			BaslerInitManager::unregister_capture();
		}
	} catch (...) {
		MUTEX_UNLOCK;
		throw;
	}
	MUTEX_UNLOCK;
	return stopped;
}

void CaptureBasler::releaseFrame() {
	MUTEX_LOCK;
	try {
		if (last_buf) {
			free(last_buf);
			last_buf = NULL;
		}
	} catch (...) {
		MUTEX_UNLOCK;
		throw;
	}
	MUTEX_UNLOCK;
}

RawImage CaptureBasler::getFrame() {
	MUTEX_LOCK;
	RawImage img;
	img.setWidth(0);
	img.setHeight(0);
	img.setColorFormat(COLOR_RGB8);
	try {
		// Keep grabbing in case of partial grabs
		int fail_count = 0;
		while (fail_count < 10
				&& (!grab_result || !grab_result->GrabSucceeded())) {
			try {
				camera->RetrieveResult(1000, grab_result,
						Pylon::TimeoutHandling_ThrowException);
			} catch (Pylon::TimeoutException& e) {
				fprintf(stderr,
						"Timeout expired in CaptureBasler::getFrame: %s\n",
						e.what());
				MUTEX_UNLOCK;
				return img;
			}
			if (!grab_result) {
				fail_count++;
				continue;
			}
			if (!grab_result->GrabSucceeded()) {
				fail_count++;
				fprintf(stderr,
						"Image grab failed in CaptureBasler::getFrame: %s\n",
						grab_result->GetErrorDescription().c_str());
			}
		}
		if (fail_count == 10) {
			fprintf(stderr,
					"Maximum retry count for image grabbing (%d) exceeded in capture_basler",
					fail_count);
			MUTEX_UNLOCK;
			return img;
		}
        timeval tv;
        gettimeofday(&tv, 0);
        img.setTime((double) tv.tv_sec + (tv.tv_usec / 1000000.0));

		Pylon::CPylonImage capture;

		// Convert to RGB8 format
		converter.Convert(capture, grab_result);

		img.setWidth(capture.GetWidth());
		img.setHeight(capture.GetHeight());
		unsigned char* buf = (unsigned char*) malloc(capture.GetImageSize());
		memcpy(buf, capture.GetBuffer(), capture.GetImageSize());
		img.setData(buf);
		last_buf = buf;

		if(Pylon::PayloadType_ChunkData != grab_result->GetPayloadType()){
		    std::cerr<<" Unexpected payload type received"<<std::endl;
		}
		if(GenApi::IsReadable(grab_result->ChunkTimestamp)){
		    std::cout<<"Basler timestamp: "<<grab_result->ChunkTimestamp.GetValue()<<std::endl;
		}else{
		    std::cerr<<"Could not read TimeStamp!"<<std::endl;
		}
		if(GenApi::IsReadable(grab_result->ChunkFramecounter)){
		    std::cout<<"Basler framecount: "<<grab_result->ChunkFramecounter.GetValue()<<std::endl;
		}else{
		    std::cerr<<"Could not read FrameCount"<<std::endl;
		}

		// Original buffer is not needed anymore, it has been copied to img
		grab_result.Release();
	} catch (Pylon::GenericException& e) {
		fprintf(stderr, "Exception while grabbing a frame: %s\n", e.what());
		MUTEX_UNLOCK;
		throw;
	} catch (...) {
		// Make sure the mutex is unlocked before propagating
        printf("Uncaught exception!\n");
		MUTEX_UNLOCK;
		throw;
	}
	MUTEX_UNLOCK;
	return img;
}

string CaptureBasler::getCaptureMethodName() const {
	return "Basler";
}

bool CaptureBasler::copyAndConvertFrame(const RawImage & src,
		RawImage & target) {
	MUTEX_LOCK;
	try {
		target.ensure_allocation(COLOR_RGB8, src.getWidth(), src.getHeight());
		target.setTime(src.getTime());
		memcpy(target.getData(), src.getData(), src.getNumBytes());
	} catch (...) {
		MUTEX_UNLOCK;
		throw;
	}
	MUTEX_UNLOCK;
	return true;
}

void CaptureBasler::readAllParameterValues() {
	MUTEX_LOCK;
	try {
		if (!camera)
			return;
		bool was_open = camera->IsOpen();
		if (!was_open) {
			camera->Open();
		}
		camera->BalanceRatioSelector.SetValue(
				Basler_GigECamera::BalanceRatioSelector_Red);
		v_balance_ratio_red->setInt(camera->BalanceRatioRaw.GetValue());
		camera->BalanceRatioSelector.SetValue(
				Basler_GigECamera::BalanceRatioSelector_Green);
		v_balance_ratio_green->setInt(camera->BalanceRatioRaw.GetValue());
		camera->BalanceRatioSelector.SetValue(
				Basler_GigECamera::BalanceRatioSelector_Blue);
		v_balance_ratio_blue->setInt(camera->BalanceRatioRaw.GetValue());

		v_auto_gain->setBool(camera->GainAuto.GetValue() == Basler_GigECamera::GainAuto_Continuous);
		v_gain->setDouble(camera->GainRaw.GetValue());
		v_gamma_enable->setBool(camera->GammaEnable.GetValue());
		v_gamma->setDouble(camera->Gamma.GetValue());

		v_auto_exposure->setBool(camera->ExposureAuto.GetValue() == Basler_GigECamera::ExposureAuto_Continuous);
		v_manual_exposure->setDouble(camera->ExposureTimeAbs.GetValue());
	} catch (const Pylon::GenericException& e) {
		fprintf(stderr, "Exception reading parameter values: %s\n", e.what());
		MUTEX_UNLOCK;
		return;
	} catch (...) {
		MUTEX_UNLOCK;
		throw;
	}
	MUTEX_UNLOCK;
}

void CaptureBasler::resetCamera(unsigned int new_id) {
	bool restart = is_capturing;
	if (restart) {
		stopCapture();
	}
	current_id = new_id;
	if (restart) {
		startCapture();
	}
}

void CaptureBasler::writeParameterValues(VarList* vars) {
	if (vars != this->settings) {
		return;
	}
	bool restart = is_capturing;
	//if (restart) {
	//	_stopCapture();
	//}
	MUTEX_LOCK;
	try {
		current_id = v_camera_id->get();

		MUTEX_UNLOCK;
		resetCamera(v_camera_id->get()); // locks itself
		MUTEX_LOCK;

        if (camera != NULL) {
            camera->Open();

            camera->BalanceRatioSelector.SetValue(
                    Basler_GigECamera::BalanceRatioSelector_Red);
            camera->BalanceRatioRaw.SetValue(v_balance_ratio_red->get());
            camera->BalanceRatioSelector.SetValue(
                    Basler_GigECamera::BalanceRatioSelector_Green);
            camera->BalanceRatioRaw.SetValue(v_balance_ratio_green->get());
            camera->BalanceRatioSelector.SetValue(
                    Basler_GigECamera::BalanceRatioSelector_Blue);
            camera->BalanceRatioRaw.SetValue(v_balance_ratio_blue->get());
            camera->BalanceWhiteAuto.SetValue(
                    Basler_GigECamera::BalanceWhiteAuto_Once);

            if (v_auto_gain->getBool()) {
                camera->GainAuto.SetValue(Basler_GigECamera::GainAuto_Continuous);
            } else {
                camera->GainAuto.SetValue(Basler_GigECamera::GainAuto_Off);
                camera->GainRaw.SetValue(v_gain->getInt());
            }

            if (v_gamma_enable->getBool()) {
                camera->GammaEnable.SetValue(true);
                camera->Gamma.SetValue(v_gamma->getDouble());
            } else {
                camera->GammaEnable.SetValue(false);
            }

            if (v_auto_exposure->getBool()) {
                camera->ExposureAuto.SetValue(
                        Basler_GigECamera::ExposureAuto_Continuous);
            } else {
                camera->ExposureAuto.SetValue(Basler_GigECamera::ExposureAuto_Off);
                camera->ExposureTimeAbs.SetValue(v_manual_exposure->getDouble());
            }
        }
	} catch (const Pylon::GenericException& e) {
		MUTEX_UNLOCK;
		fprintf(stderr, "Error writing parameter values: %s\n", e.what());
		throw;
	} catch (...) {
		MUTEX_UNLOCK;
		throw;
	}
	MUTEX_UNLOCK;
	//if (restart) {
	//	startCapture();
	//}
}

void CaptureBasler::mvc_connect(VarList * group) {
	vector<VarType *> v = group->getChildren();
	for (unsigned int i = 0; i < v.size(); i++) {
	connect(v[i],SIGNAL(wasEdited(VarType *)),group,SLOT(mvcEditCompleted()));
}
connect(group,SIGNAL(wasEdited(VarType *)),this,SLOT(changed(VarType *)));
}

void CaptureBasler::changed(VarType * group) {
if (group->getType() == VARTYPE_ID_LIST) {
writeParameterValues(dynamic_cast<VarList*>(group));
}
}

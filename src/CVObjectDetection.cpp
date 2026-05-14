/**
 * @file
 * @brief Source file for CVObjectDetection class
 * @author Jonathan Thomas <jonathan@openshot.org>
 * @author Brenno Caldato <brenno.caldato@outlook.com>
 *
 * @ref License
 */

// Copyright (c) 2008-2019 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <fstream>
#include <iomanip>
#include <iostream>
#include <algorithm>

#include "CVObjectDetection.h"
#include "Exceptions.h"

#include "objdetectdata.pb.h"
#include <google/protobuf/util/time_util.h>

using namespace std;
using namespace openshot;
using google::protobuf::util::TimeUtil;

namespace {

std::string LoadONNXModel(std::string modelPath, cv::dnn::Net *net)
{
#if CV_VERSION_MAJOR < 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR < 3)
    return std::string("Failed to load ONNX model: YOLOv5 requires OpenCV 4.3.0 or newer. "
                       "This OpenCV build is ") + CV_VERSION + ".";
#else
    try {
        cv::dnn::Net loaded_net = cv::dnn::readNetFromONNX(modelPath);
        if (net) {
            *net = loaded_net;
        }
        return "";
    } catch (const cv::Exception& e) {
        std::string error_text = std::string("Failed to load ONNX model: ") + e.what();
        if (error_text.find("Unsupported data type: FLOAT16") != std::string::npos) {
            error_text = "Failed to load ONNX model: FLOAT16 is not supported by this OpenCV build. "
                         "Please use an FP32 ONNX model.";
        }
        return error_text;
    } catch (const std::exception& e) {
        return std::string("Failed to load ONNX model: ") + e.what();
    } catch (...) {
        return "Failed to load ONNX model: unknown error";
    }
#endif
}

}

CVObjectDetection::CVObjectDetection(std::string processInfoJson, ProcessingController &processingController)
: processingController(&processingController), processingDevice("CPU"), inpWidth(640), inpHeight(640){
    confThreshold = 0.25;
    nmsThreshold = 0.1;
    SetJson(processInfoJson);
}

std::string CVObjectDetection::ValidateONNXModel(std::string modelPath)
{
    return LoadONNXModel(modelPath, nullptr);
}

void CVObjectDetection::setProcessingDevice(){
    if(processingDevice == "GPU"){
        try {
            const std::vector<cv::dnn::Target> targets = cv::dnn::getAvailableTargets(cv::dnn::DNN_BACKEND_CUDA);
            if (std::find(targets.begin(), targets.end(), cv::dnn::DNN_TARGET_CUDA) != targets.end()) {
                net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
                net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
                return;
            }
        } catch (const cv::Exception&) {
        }
        processingDevice = "CPU";
    }

    if(processingDevice == "CPU"){
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    }
}

void CVObjectDetection::detectObjectsClip(openshot::Clip &video, size_t _start, size_t _end, bool process_interval)
{

    start = _start; end = _end;

    video.Open();

    if(error){
        return;
    }

    processingController->SetError(false, "");

    if(modelPath.empty()) {
        processingController->SetError(true, "Missing path to YOLOv5 ONNX model file");
        error = true;
        return;
    }
    if(classesFile.empty()) {
        processingController->SetError(true, "Missing path to class name file");
        error = true;
        return;
    }

    std::ifstream model_file(modelPath);
    if(!model_file.good()){
        processingController->SetError(true, "Incorrect path to YOLOv5 ONNX model file");
        error = true;
        return;
    }
    std::ifstream classes_file(classesFile);
    if(!classes_file.good()){
        processingController->SetError(true, "Incorrect path to class name file");
        error = true;
        return;
    }

    // Load names of classes
    classNames.clear();
    std::string line;
    while (std::getline(classes_file, line)) classNames.push_back(line);

    // Load the network
    std::string error_text = LoadONNXModel(modelPath, &net);
    if (!error_text.empty()) {
        processingController->SetError(true, error_text);
        error = true;
        return;
    }
    setProcessingDevice();

    size_t frame_number;
    if(!process_interval || end <= 1 || end-start == 0){
        // Get total number of frames in video
        start = (int)(video.Start() * video.Reader()->info.fps.ToFloat());
        end = (int)(video.End() * video.Reader()->info.fps.ToFloat());
    }

    for (frame_number = start; frame_number <= end; frame_number++)
    {
         // Stop the feature tracker process
        if(processingController->ShouldStop()){
            return;
        }

        std::shared_ptr<openshot::Frame> f = video.GetFrame(frame_number);

        // Grab OpenCV Mat image
        cv::Mat cvimage = f->GetImageCV();

        DetectObjects(cvimage, frame_number);

        // Update progress
        processingController->SetProgress(uint(100*(frame_number-start)/(end-start)));

    }
}

void CVObjectDetection::DetectObjects(const cv::Mat &frame, size_t frameId){
    // Get frame as OpenCV Mat
    cv::Mat blob;

    // Create a 4D blob from the frame.
    cv::dnn::blobFromImage(frame, blob, 1/255.0, cv::Size(inpWidth, inpHeight), cv::Scalar(0,0,0), true, false);

    std::vector<cv::Mat> outs;
    try {
        // Sets the input to the network
        net.setInput(blob);
        // Runs the forward pass to get output of the output layers
        net.forward(outs, getOutputsNames(net));
    } catch (const cv::Exception& e) {
        processingController->SetError(true, std::string("Object detection inference failed: ") + e.what());
        error = true;
        return;
    }

    // Remove the bounding boxes with low confidence
    postprocess(frame.size(), outs, frameId);

}


// Remove the bounding boxes with low confidence using non-maxima suppression
void CVObjectDetection::postprocess(const cv::Size &frameDims, const std::vector<cv::Mat>& outs, size_t frameId)
{
    std::vector<int> classIds;
    std::vector<float> confidences;
    std::vector<cv::Rect> boxes;
    std::vector<std::vector<ClassScore>> detectionClassScores;
    std::vector<int> objectIds;
    const int maxClassCandidates = 5;

    for (size_t i = 0; i < outs.size(); ++i) {
        cv::Mat det = outs[i];

        // YOLOv5 ONNX output is usually [1, num_boxes, num_classes + 5].
        if (det.dims == 3) {
            det = det.reshape(1, det.size[1]);
        }
        if (det.dims != 2 || det.cols < 6) {
            continue;
        }

        const float xFactor = static_cast<float>(frameDims.width) / static_cast<float>(inpWidth);
        const float yFactor = static_cast<float>(frameDims.height) / static_cast<float>(inpHeight);

        float* data = reinterpret_cast<float*>(det.data);
        for (int j = 0; j < det.rows; ++j, data += det.cols) {
            std::vector<ClassScore> rowClassScores;
            rowClassScores.reserve(maxClassCandidates);
            for (int classIndex = 5; classIndex < det.cols; ++classIndex) {
                const float classConfidence = data[classIndex] * data[4];
                if (rowClassScores.size() < static_cast<size_t>(maxClassCandidates)) {
                    rowClassScores.emplace_back(classIndex - 5, classConfidence);
                    std::sort(rowClassScores.begin(), rowClassScores.end(),
                        [](const ClassScore& a, const ClassScore& b) { return a.score > b.score; });
                } else if (classConfidence > rowClassScores.back().score) {
                    rowClassScores.back() = ClassScore(classIndex - 5, classConfidence);
                    std::sort(rowClassScores.begin(), rowClassScores.end(),
                        [](const ClassScore& a, const ClassScore& b) { return a.score > b.score; });
                }
            }
            if (rowClassScores.empty()) {
                continue;
            }

            float confidence = rowClassScores.front().score;

            if (confidence > confThreshold) {
                int centerX = 0;
                int centerY = 0;
                int width = 0;
                int height = 0;

                if (data[0] > 1.0f || data[1] > 1.0f || data[2] > 1.0f || data[3] > 1.0f) {
                    centerX = static_cast<int>(data[0] * xFactor);
                    centerY = static_cast<int>(data[1] * yFactor);
                    width = static_cast<int>(data[2] * xFactor);
                    height = static_cast<int>(data[3] * yFactor);
                } else {
                    centerX = static_cast<int>(data[0] * frameDims.width);
                    centerY = static_cast<int>(data[1] * frameDims.height);
                    width = static_cast<int>(data[2] * frameDims.width);
                    height = static_cast<int>(data[3] * frameDims.height);
                }

                int left = centerX - width / 2;
                int top = centerY - height / 2;

                classIds.push_back(rowClassScores.front().classId);
                confidences.push_back(confidence);
                boxes.push_back(cv::Rect(left, top, width, height));
                detectionClassScores.push_back(rowClassScores);
            }
        }
    }

    // Perform non maximum suppression to eliminate redundant overlapping boxes with
    // lower confidences
    std::vector<int> indices;
    cv::dnn::NMSBoxes(boxes, confidences, confThreshold, nmsThreshold, indices);

    // Pass boxes to SORT algorithm
    std::vector<cv::Rect> sortBoxes;
    std::vector<float> sortConfidences;
    std::vector<int> sortClassIds;
    std::vector<std::vector<ClassScore>> sortClassScores;
    for(auto index : indices) {
        sortBoxes.push_back(boxes[index]);
        sortConfidences.push_back(confidences[index]);
        sortClassIds.push_back(classIds[index]);
        sortClassScores.push_back(detectionClassScores[index]);
    }
    sort.update(sortBoxes, frameId, sqrt(pow(frameDims.width,2) + pow(frameDims.height, 2)), sortConfidences, sortClassIds, sortClassScores);

    // Clear data vectors
    boxes.clear(); confidences.clear(); classIds.clear(); objectIds.clear();
    // Get SORT predicted boxes
    for(auto TBox : sort.frameTrackingResult){
        if(TBox.frame == frameId){
            boxes.push_back(TBox.box);
            confidences.push_back(TBox.confidence);
            classIds.push_back(TBox.classId);
            objectIds.push_back(TBox.id);
        }
    }

    // Remove boxes based on controids distance
    for(uint i = 0; i<boxes.size(); i++){
        for(uint j = i+1; j<boxes.size(); j++){
            int xc_1 = boxes[i].x + (int)(boxes[i].width/2), yc_1 = boxes[i].y + (int)(boxes[i].height/2);
            int xc_2 = boxes[j].x + (int)(boxes[j].width/2), yc_2 = boxes[j].y + (int)(boxes[j].height/2);

            if(fabs(xc_1 - xc_2) < 10 && fabs(yc_1 - yc_2) < 10){
                if(classIds[i] == classIds[j]){
                    if(confidences[i] >= confidences[j]){
                        boxes.erase(boxes.begin() + j);
                        classIds.erase(classIds.begin() + j);
                        confidences.erase(confidences.begin() + j);
                        objectIds.erase(objectIds.begin() + j);
                        break;
                    }
                    else{
                        boxes.erase(boxes.begin() + i);
                        classIds.erase(classIds.begin() + i);
                        confidences.erase(confidences.begin() + i);
                        objectIds.erase(objectIds.begin() + i);
                        i = 0;
                        break;
                    }
                }
            }
        }
    }

    // Remove boxes based in IOU score
    for(uint i = 0; i<boxes.size(); i++){
        for(uint j = i+1; j<boxes.size(); j++){

            if( iou(boxes[i], boxes[j])){
                if(classIds[i] == classIds[j]){
                    if(confidences[i] >= confidences[j]){
                        boxes.erase(boxes.begin() + j);
                        classIds.erase(classIds.begin() + j);
                        confidences.erase(confidences.begin() + j);
                        objectIds.erase(objectIds.begin() + j);
                        break;
                    }
                    else{
                        boxes.erase(boxes.begin() + i);
                        classIds.erase(classIds.begin() + i);
                        confidences.erase(confidences.begin() + i);
                        objectIds.erase(objectIds.begin() + i);
                        i = 0;
                        break;
                    }
                }
            }
        }
    }

    // Normalize boxes coordinates
    std::vector<cv::Rect_<float>> normalized_boxes;
    for(auto box : boxes){
        cv::Rect_<float> normalized_box;
        normalized_box.x = (box.x)/(float)frameDims.width;
        normalized_box.y = (box.y)/(float)frameDims.height;
        normalized_box.width = (box.width)/(float)frameDims.width;
        normalized_box.height = (box.height)/(float)frameDims.height;
        normalized_boxes.push_back(normalized_box);
    }

    detectionsData[frameId] = CVDetectionData(classIds, confidences, normalized_boxes, frameId, objectIds);
}

// Compute IOU between 2 boxes
bool CVObjectDetection::iou(cv::Rect pred_box, cv::Rect sort_box){
    // Determine the (x, y)-coordinates of the intersection rectangle
	int xA = std::max(pred_box.x, sort_box.x);
	int yA = std::max(pred_box.y, sort_box.y);
	int xB = std::min(pred_box.x + pred_box.width, sort_box.x + sort_box.width);
	int yB = std::min(pred_box.y + pred_box.height, sort_box.y + sort_box.height);

	// Compute the area of intersection rectangle
	int interArea = std::max(0, xB - xA + 1) * std::max(0, yB - yA + 1);

	// Compute the area of both the prediction and ground-truth rectangles
	int boxAArea = (pred_box.width + 1) * (pred_box.height + 1);
	int boxBArea = (sort_box.width + 1) * (sort_box.height + 1);

	// Compute the intersection over union by taking the intersection
	float iou = interArea / (float)(boxAArea + boxBArea - interArea);

    // If IOU is above this value the boxes are very close (probably a variation of the same bounding box)
    if(iou > 0.5)
        return true;
    return false;
}

// Get the names of the output layers
std::vector<cv::String> CVObjectDetection::getOutputsNames(const cv::dnn::Net& net)
{
    //Get the indices of the output layers, i.e. the layers with unconnected outputs
    std::vector<int> outLayers = net.getUnconnectedOutLayers();

    //get the names of all the layers in the network
    std::vector<cv::String> layersNames = net.getLayerNames();

    // Get the names of the output layers in names
    std::vector<cv::String> names;
    names.resize(outLayers.size());
    for (size_t i = 0; i < outLayers.size(); ++i)
        names[i] = layersNames[outLayers[i] - 1];
    return names;
}

CVDetectionData CVObjectDetection::GetDetectionData(size_t frameId){
    // Check if the stabilizer info for the requested frame exists
    if ( detectionsData.find(frameId) == detectionsData.end() ) {

        return CVDetectionData();
    } else {

        return detectionsData[frameId];
    }
}

bool CVObjectDetection::SaveObjDetectedData(){
    if(protobuf_data_path.empty()) {
        cerr << "Missing path to object detection protobuf data file." << endl;
        return false;
    }

    // Create tracker message
    pb_objdetect::ObjDetect objMessage;

    //Save class names in protobuf message
    for(int i = 0; i<classNames.size(); i++){
        std::string* className = objMessage.add_classnames();
        className->assign(classNames.at(i));
    }

    // Iterate over all frames data and save in protobuf message
    for(std::map<size_t,CVDetectionData>::iterator it=detectionsData.begin(); it!=detectionsData.end(); ++it){
        CVDetectionData dData = it->second;
        AddFrameDataToProto(objMessage.add_frame(), dData);
    }

    // Add timestamp
    *objMessage.mutable_last_updated() = TimeUtil::SecondsToTimestamp(time(NULL));

    {
        // Write the new message to disk.
        std::fstream output(protobuf_data_path, ios::out | ios::trunc | ios::binary);
        if (!objMessage.SerializeToOstream(&output)) {
        cerr << "Failed to write protobuf message." << endl;
        return false;
        }
    }

    // Delete all global objects allocated by libprotobuf.
    google::protobuf::ShutdownProtobufLibrary();

    return true;

}

// Add frame object detection into protobuf message.
void CVObjectDetection::AddFrameDataToProto(pb_objdetect::Frame* pbFrameData, CVDetectionData& dData) {

    // Save frame number and rotation
    pbFrameData->set_id(dData.frameId);

    for(size_t i = 0; i < dData.boxes.size(); i++){
        pb_objdetect::Frame_Box* box = pbFrameData->add_bounding_box();

        // Save bounding box data
        box->set_x(dData.boxes.at(i).x);
        box->set_y(dData.boxes.at(i).y);
        box->set_w(dData.boxes.at(i).width);
        box->set_h(dData.boxes.at(i).height);
        box->set_classid(dData.classIds.at(i));
        box->set_confidence(dData.confidences.at(i));
        box->set_objectid(dData.objectIds.at(i));

    }
}

// Load JSON string into this object
void CVObjectDetection::SetJson(const std::string value) {
	// Parse JSON string into JSON objects
	try
	{
		const Json::Value root = openshot::stringToJson(value);
		// Set all values that match

		SetJsonValue(root);
	}
	catch (const std::exception& e)
	{
		// Error parsing JSON (or missing keys)
		// throw InvalidJSON("JSON is invalid (missing keys or invalid data types)");
        std::cout<<"JSON is invalid (missing keys or invalid data types)"<<std::endl;
	}
}

// Load Json::Value into this object
void CVObjectDetection::SetJsonValue(const Json::Value root) {

	// Set data from Json (if key is found)
	if (!root["protobuf_data_path"].isNull()){
		protobuf_data_path = (root["protobuf_data_path"].asString());
	}

    if (!root["processing-device"].isNull()){
		processingDevice = (root["processing-device"].asString());
	}
    if (!root["processing_device"].isNull()){
		processingDevice = (root["processing_device"].asString());
	}
    if (!root["class-names"].isNull()){
		classesFile = (root["class-names"].asString());
	}
    if (!root["classes_file"].isNull()){
		classesFile = (root["classes_file"].asString());
	}
    if (!root["model"].isNull()){
        modelPath = (root["model"].asString());
    }
    if (!root["model_path"].isNull()){
        modelPath = (root["model_path"].asString());
    }
    if (!root["input-width"].isNull()){
        inpWidth = root["input-width"].asInt();
    }
    if (!root["input_width"].isNull()){
        inpWidth = root["input_width"].asInt();
    }
    if (!root["input-height"].isNull()){
        inpHeight = root["input-height"].asInt();
    }
    if (!root["input_height"].isNull()){
        inpHeight = root["input_height"].asInt();
    }
    if (!root["confidence-threshold"].isNull()){
        confThreshold = root["confidence-threshold"].asFloat();
    }
    if (!root["confidence_threshold"].isNull()){
        confThreshold = root["confidence_threshold"].asFloat();
    }
    if (!root["nms-threshold"].isNull()){
        nmsThreshold = root["nms-threshold"].asFloat();
    }
    if (!root["nms_threshold"].isNull()){
        nmsThreshold = root["nms_threshold"].asFloat();
    }
}

/*
||||||||||||||||||||||||||||||||||||||||||||||||||
                ONLY FOR MAKE TEST
||||||||||||||||||||||||||||||||||||||||||||||||||
*/

// Load protobuf data file
bool CVObjectDetection::_LoadObjDetectdData(){
    if(protobuf_data_path.empty()) {
        cerr << "Missing path to object detection protobuf data file." << endl;
        return false;
    }

    // Create tracker message
    pb_objdetect::ObjDetect objMessage;

    {
        // Read the existing tracker message.
        fstream input(protobuf_data_path, ios::in | ios::binary);
        if (!objMessage.ParseFromIstream(&input)) {
            cerr << "Failed to parse protobuf message." << endl;
            return false;
        }
    }

    // Make sure classNames and detectionsData are empty
    classNames.clear(); detectionsData.clear();

    // Get all classes names and assign a color to them
    for(int i = 0; i < objMessage.classnames_size(); i++){
        classNames.push_back(objMessage.classnames(i));
    }

    // Iterate over all frames of the saved message
    for (size_t i = 0; i < objMessage.frame_size(); i++) {
        // Create protobuf message reader
        const pb_objdetect::Frame& pbFrameData = objMessage.frame(i);

        // Get frame Id
        size_t id = pbFrameData.id();

        // Load bounding box data
        const google::protobuf::RepeatedPtrField<pb_objdetect::Frame_Box > &pBox = pbFrameData.bounding_box();

        // Construct data vectors related to detections in the current frame
        std::vector<int> classIds;
        std::vector<float> confidences;
        std::vector<cv::Rect_<float>> boxes;
        std::vector<int> objectIds;

        for(int i = 0; i < pbFrameData.bounding_box_size(); i++){
            // Get bounding box coordinates
            float x = pBox.Get(i).x(); float y = pBox.Get(i).y();
            float w = pBox.Get(i).w(); float h = pBox.Get(i).h();
            // Create OpenCV rectangle with the bouding box info
            cv::Rect_<float> box(x, y, w, h);

            // Get class Id (which will be assign to a class name) and prediction confidence
            int classId = pBox.Get(i).classid(); float confidence = pBox.Get(i).confidence();
            // Get object Id
            int objectId = pBox.Get(i).objectid();

            // Push back data into vectors
            boxes.push_back(box); classIds.push_back(classId); confidences.push_back(confidence);
            objectIds.push_back(objectId);
        }

        // Assign data to object detector map
        detectionsData[id] = CVDetectionData(classIds, confidences, boxes, id, objectIds);
    }

    // Delete all global objects allocated by libprotobuf.
    google::protobuf::ShutdownProtobufLibrary();

    return true;
}

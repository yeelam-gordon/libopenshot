/**
 * @file
 * @brief Source file for CVObjectMask class
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2026 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "CVObjectMask.h"

#include "Exceptions.h"
#include "objdetectdata.pb.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <iostream>
#include <cstring>
#include <numeric>

#include <google/protobuf/util/time_util.h>

using namespace openshot;
using google::protobuf::util::TimeUtil;

namespace {

std::string LoadONNXModel(const std::string& modelPath, cv::dnn::Net* net)
{
    try {
        cv::dnn::Net loadedNet = cv::dnn::readNetFromONNX(modelPath);
        if (net)
            *net = loadedNet;
        return "";
    } catch (const cv::Exception& e) {
        return std::string("Failed to load ONNX model: ") + e.what();
    } catch (const std::exception& e) {
        return std::string("Failed to load ONNX model: ") + e.what();
    }
}

std::vector<uint32_t> EncodeBinaryMaskRLE(const cv::Mat& mask)
{
    std::vector<uint32_t> rle;
    if (mask.empty())
        return rle;

    uint8_t current = 0;
    uint32_t count = 0;
    for (int y = 0; y < mask.rows; ++y) {
        const uint8_t* row = mask.ptr<uint8_t>(y);
        for (int x = 0; x < mask.cols; ++x) {
            const uint8_t value = row[x] ? 1 : 0;
            if (value == current) {
                ++count;
            } else {
                rle.push_back(count);
                current = value;
                count = 1;
            }
        }
    }
    rle.push_back(count);
    return rle;
}

struct EfficientSamPreprocessResult {
    cv::Mat blob;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
};

EfficientSamPreprocessResult MakeEfficientSamBlob(const cv::Mat& bgr, int modelSize)
{
    EfficientSamPreprocessResult result;
    result.scaleX = static_cast<float>(modelSize) / static_cast<float>(bgr.cols);
    result.scaleY = static_cast<float>(modelSize) / static_cast<float>(bgr.rows);

    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(modelSize, modelSize), 0, 0, cv::INTER_LINEAR);

    const int shape[] = {1, 3, modelSize, modelSize};
    result.blob = cv::Mat(4, shape, CV_32F);
    float* dst = result.blob.ptr<float>();

    for (int y = 0; y < resized.rows; ++y) {
        const cv::Vec3b* row = resized.ptr<cv::Vec3b>(y);
        for (int x = 0; x < resized.cols; ++x) {
            const float rgb[] = {
                static_cast<float>(row[x][2]) / 255.0f,
                static_cast<float>(row[x][1]) / 255.0f,
                static_cast<float>(row[x][0]) / 255.0f,
            };
            for (int c = 0; c < 3; ++c)
                dst[(c * modelSize + y) * modelSize + x] = rgb[c];
        }
    }

    return result;
}

cv::Rect_<float> NormalizedBoundingBox(const cv::Mat& mask)
{
    std::vector<cv::Point> points;
    cv::findNonZero(mask, points);
    if (points.empty())
        return {};

    cv::Rect rect = cv::boundingRect(points);
    return cv::Rect_<float>(
        rect.x / static_cast<float>(mask.cols),
        rect.y / static_cast<float>(mask.rows),
        rect.width / static_cast<float>(mask.cols),
        rect.height / static_cast<float>(mask.rows));
}

cv::Mat EfficientSamMaskToFrameMask(const cv::Mat& modelMask, const cv::Size& frameSize, float maskThreshold)
{
    cv::Mat fullSize;
    cv::resize(modelMask, fullSize, frameSize, 0, 0, cv::INTER_LINEAR);

    cv::Mat binary;
    cv::threshold(fullSize, binary, maskThreshold, 255.0, cv::THRESH_BINARY);
    binary.convertTo(binary, CV_8U);
    return binary;
}

cv::Mat MakeEfficientSamPromptBlob(
    const CVObjectMaskPromptSet& prompts,
    const EfficientSamPreprocessResult& prep,
    int promptSlots,
    std::vector<cv::Point>& backgroundPoints)
{
    const int coordsShape[] = {1, 1, promptSlots, 2};
    cv::Mat pointCoords(4, coordsShape, CV_32F, cv::Scalar(0.0f));

    float* coords = pointCoords.ptr<float>();
    int promptIndex = 0;
    if (prompts.hasRect && promptSlots >= 2) {
        coords[0] = prompts.rectTopLeft.x * prep.scaleX;
        coords[1] = prompts.rectTopLeft.y * prep.scaleY;
        coords[2] = prompts.rectBottomRight.x * prep.scaleX;
        coords[3] = prompts.rectBottomRight.y * prep.scaleY;
        promptIndex = 2;
    }
    for (const auto& point : prompts.positivePoints) {
        if (promptIndex >= promptSlots)
            break;
        coords[promptIndex * 2] = point.x * prep.scaleX;
        coords[promptIndex * 2 + 1] = point.y * prep.scaleY;
        ++promptIndex;
    }
    for (const auto& point : prompts.negativePoints) {
        backgroundPoints.emplace_back(
            static_cast<int>(std::lround(point.x * prep.scaleX)),
            static_cast<int>(std::lround(point.y * prep.scaleY)));
    }

    return pointCoords;
}

cv::Mat MakeEfficientSamLabelBlob(const CVObjectMaskPromptSet& prompts, int promptSlots)
{
    const int labelsShape[] = {1, 1, promptSlots, 1};
    cv::Mat pointLabels(4, labelsShape, CV_32F, cv::Scalar(-1.0f));

    float* labels = pointLabels.ptr<float>();
    int promptIndex = 0;
    if (prompts.hasRect && promptSlots >= 2) {
        labels[0] = 2.0f;
        labels[1] = 3.0f;
        promptIndex = 2;
    }
    for (size_t i = 0; i < prompts.positivePoints.size() && promptIndex < promptSlots; ++i, ++promptIndex)
        labels[promptIndex] = 1.0f;

    return pointLabels;
}

cv::Mat SelectEfficientSamMask(const cv::Mat& outputMasks, const cv::Mat& iouPredictions,
                               const std::vector<cv::Point>& backgroundPoints, float maskThreshold)
{
    if (outputMasks.dims != 5 || iouPredictions.empty())
        return cv::Mat();

    const int candidateCount = outputMasks.size[2];
    const int maskHeight = outputMasks.size[3];
    const int maskWidth = outputMasks.size[4];
    const float* ious = iouPredictions.ptr<float>();

    std::vector<int> order(candidateCount);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return ious[a] > ious[b];
    });

    const float* masks = outputMasks.ptr<float>();
    const size_t candidatePixels = static_cast<size_t>(maskHeight) * static_cast<size_t>(maskWidth);
    cv::Mat fallback;
    for (int candidate : order) {
        cv::Mat mask(maskHeight, maskWidth, CV_32F,
                     const_cast<float*>(masks + static_cast<size_t>(candidate) * candidatePixels));
        if (fallback.empty())
            fallback = mask.clone();

        bool containsBackground = false;
        for (const cv::Point& point : backgroundPoints) {
            const int x = std::max(0, std::min(maskWidth - 1, point.x));
            const int y = std::max(0, std::min(maskHeight - 1, point.y));
            if (mask.at<float>(y, x) >= maskThreshold) {
                containsBackground = true;
                break;
            }
        }
        if (!containsBackground)
            return mask.clone();
    }
    return fallback;
}

CVObjectMaskFrameData FrameDataFromMask(const cv::Mat& mask, size_t frameId, float score)
{
    CVObjectMaskFrameData frameData;
    frameData.frameId = frameId;
    frameData.objectId = 1;
    if (mask.empty())
        return frameData;

    frameData.score = score;
    frameData.width = mask.cols;
    frameData.height = mask.rows;
    frameData.rle = EncodeBinaryMaskRLE(mask);
    frameData.box = NormalizedBoundingBox(mask);
    return frameData;
}

cv::Point2f JsonPoint(const Json::Value& value)
{
    if (!value.isObject() || value["x"].isNull() || value["y"].isNull())
        return cv::Point2f(-1.0f, -1.0f);
    return cv::Point2f(value["x"].asFloat(), value["y"].asFloat());
}

bool IsValidPoint(const cv::Point2f& point)
{
    return point.x >= 0.0f && point.y >= 0.0f;
}

void AppendJsonPoints(const Json::Value& values, std::vector<cv::Point2f>& points)
{
    if (!values.isArray())
        return;
    for (const auto& value : values) {
        cv::Point2f point = JsonPoint(value);
        if (IsValidPoint(point))
            points.push_back(point);
    }
}

size_t JsonFrameNumber(const std::string& frameName)
{
    try {
        return static_cast<size_t>(std::max(0, std::stoi(frameName)));
    } catch (...) {
        return 0;
    }
}

void ApplyRectJson(const Json::Value& rect, CVObjectMaskPromptSet& prompts)
{
    if (!rect.isObject() || rect["x1"].isNull() || rect["y1"].isNull() ||
        rect["x2"].isNull() || rect["y2"].isNull()) {
        return;
    }

    prompts.rectTopLeft.x = std::min(rect["x1"].asFloat(), rect["x2"].asFloat());
    prompts.rectTopLeft.y = std::min(rect["y1"].asFloat(), rect["y2"].asFloat());
    prompts.rectBottomRight.x = std::max(rect["x1"].asFloat(), rect["x2"].asFloat());
    prompts.rectBottomRight.y = std::max(rect["y1"].asFloat(), rect["y2"].asFloat());
    prompts.hasRect = IsValidPoint(prompts.rectTopLeft) && IsValidPoint(prompts.rectBottomRight);
}

void AppendNegativeRectCenters(const Json::Value& values, CVObjectMaskPromptSet& prompts)
{
    if (!values.isArray())
        return;
    for (const auto& rect : values) {
        if (!rect.isObject() || rect["x1"].isNull() || rect["y1"].isNull() ||
            rect["x2"].isNull() || rect["y2"].isNull()) {
            continue;
        }
        cv::Point2f center(
            (rect["x1"].asFloat() + rect["x2"].asFloat()) / 2.0f,
            (rect["y1"].asFloat() + rect["y2"].asFloat()) / 2.0f);
        if (IsValidPoint(center))
            prompts.negativePoints.push_back(center);
    }
}

CVObjectMaskPromptSet PromptSetFromJson(const Json::Value& framePayload)
{
    CVObjectMaskPromptSet prompts;
    AppendJsonPoints(framePayload["positive_points"], prompts.positivePoints);
    AppendJsonPoints(framePayload["negative_points"], prompts.negativePoints);
    if (framePayload["positive_rects"].isArray() && framePayload["positive_rects"].size() > 0)
        ApplyRectJson(framePayload["positive_rects"][0], prompts);
    AppendNegativeRectCenters(framePayload["negative_rects"], prompts);
    return prompts;
}

cv::Mat MakeXMemImageBlob(const cv::Mat& bgr)
{
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(640, 480), 0, 0, cv::INTER_LINEAR);

    const int shape[] = {1, 3, 480, 640};
    cv::Mat blob(4, shape, CV_32F);
    float* dst = blob.ptr<float>();
    const float mean[] = {0.485f, 0.456f, 0.406f};
    const float stddev[] = {0.229f, 0.224f, 0.225f};

    for (int y = 0; y < resized.rows; ++y) {
        const cv::Vec3b* row = resized.ptr<cv::Vec3b>(y);
        for (int x = 0; x < resized.cols; ++x) {
            const float rgb[] = {
                static_cast<float>(row[x][2]) / 255.0f,
                static_cast<float>(row[x][1]) / 255.0f,
                static_cast<float>(row[x][0]) / 255.0f,
            };
            for (int c = 0; c < 3; ++c)
                dst[(c * 480 + y) * 640 + x] = (rgb[c] - mean[c]) / stddev[c];
        }
    }

    return blob;
}

cv::Mat MakeScalarBlob(float value)
{
    const int shape[] = {1};
    return cv::Mat(1, shape, CV_32F, cv::Scalar(value));
}

cv::Mat MakeXMemMaskBlob(const cv::Mat& mask)
{
    cv::Mat resized;
    cv::resize(mask, resized, cv::Size(640, 480), 0, 0, cv::INTER_NEAREST);
    const int shape[] = {1, 1, 480, 640};
    cv::Mat blob(4, shape, CV_32F, cv::Scalar(0.0f));
    float* dst = blob.ptr<float>();
    for (int y = 0; y < resized.rows; ++y) {
        const uint8_t* row = resized.ptr<uint8_t>(y);
        for (int x = 0; x < resized.cols; ++x)
            dst[y * 640 + x] = row[x] ? 1.0f : 0.0f;
    }
    return blob;
}

cv::Mat BinaryMaskFromXMemProb(const cv::Mat& prob)
{
    cv::Mat mask(480, 640, CV_8U, cv::Scalar(0));
    const float* src = prob.ptr<float>();
    for (int y = 0; y < mask.rows; ++y) {
        uint8_t* row = mask.ptr<uint8_t>(y);
        for (int x = 0; x < mask.cols; ++x)
            row[x] = src[y * 640 + x] >= 0.5f ? 255 : 0;
    }
    return mask;
}

cv::Mat AggregateXMemForegroundProb(const cv::Mat& rawProb)
{
    const int shape[] = {1, 1, 480, 640};
    cv::Mat output(4, shape, CV_32F);
    const float* src = rawProb.ptr<float>();
    float* dst = output.ptr<float>();
    for (int i = 0; i < 480 * 640; ++i) {
        const float fg = std::min(1.0f - 1e-7f, std::max(1e-7f, src[i]));
        const float bg = std::min(1.0f - 1e-7f, std::max(1e-7f, 1.0f - fg));
        const float bgLogit = std::log(bg / (1.0f - bg));
        const float fgLogit = std::log(fg / (1.0f - fg));
        const float maxLogit = std::max(bgLogit, fgLogit);
        const float bgExp = std::exp(bgLogit - maxLogit);
        const float fgExp = std::exp(fgLogit - maxLogit);
        dst[i] = fgExp / (bgExp + fgExp);
    }
    return output;
}

cv::Mat FlattenFeature(const cv::Mat& feature, int channels)
{
    return cv::Mat(channels, 30 * 40, CV_32F, const_cast<float*>(feature.ptr<float>())).clone();
}

cv::Mat FlattenShrinkage(const cv::Mat& shrinkage)
{
    return cv::Mat(1, 30 * 40, CV_32F, const_cast<float*>(shrinkage.ptr<float>())).clone();
}

cv::Mat MatFromReadout(const cv::Mat& readout)
{
    const int shape[] = {1, 1, 512, 30, 40};
    cv::Mat output(5, shape, CV_32F);
    std::memcpy(output.ptr<float>(), readout.ptr<float>(), sizeof(float) * readout.total());
    return output;
}

class XMemPropagator {
private:
    struct MemoryFrame {
        cv::Mat key;
        cv::Mat shrinkage;
        cv::Mat value;
    };

    cv::dnn::Net encodeKey;
    cv::dnn::Net encodeValue;
    cv::dnn::Net decode;
    cv::Mat memoryKey;
    cv::Mat memoryShrinkage;
    cv::Mat memoryValue;
    cv::Mat hidden;
    std::deque<MemoryFrame> memoryFrames;
    int frameIndex = 0;
    int lastMemoryFrame = -1000000;
    int memEvery = 5;
    int maxMemoryFrames = 10;

    void RebuildMemory()
    {
        std::vector<cv::Mat> keys;
        std::vector<cv::Mat> shrinkages;
        std::vector<cv::Mat> values;
        for (const auto& frame : memoryFrames) {
            keys.push_back(frame.key);
            shrinkages.push_back(frame.shrinkage);
            values.push_back(frame.value);
        }
        if (keys.empty()) {
            memoryKey.release();
            memoryShrinkage.release();
            memoryValue.release();
            return;
        }
        cv::hconcat(keys, memoryKey);
        cv::hconcat(shrinkages, memoryShrinkage);
        cv::hconcat(values, memoryValue);
    }

    cv::Mat MatchMemory(const cv::Mat& queryKey, const cv::Mat& selection)
    {
        cv::Mat query = FlattenFeature(queryKey, 64);
        cv::Mat querySelection = FlattenFeature(selection, 64);
        cv::Mat weightedQuery = query.mul(querySelection);

        cv::Mat twoAb;
        cv::gemm(memoryKey, weightedQuery, 2.0, cv::Mat(), 0.0, twoAb, cv::GEMM_1_T);

        cv::Mat aSq;
        cv::gemm(memoryKey.mul(memoryKey), querySelection, 1.0, cv::Mat(), 0.0, aSq, cv::GEMM_1_T);

        cv::Mat querySquared = query.mul(query).mul(querySelection);
        std::vector<float> bSq(query.cols, 0.0f);
        for (int q = 0; q < query.cols; ++q) {
            double sum = 0.0;
            for (int c = 0; c < query.rows; ++c)
                sum += querySquared.at<float>(c, q);
            bSq[q] = static_cast<float>(sum);
        }

        cv::Mat similarity = twoAb - aSq;
        const float invSqrtKeyDim = 1.0f / std::sqrt(64.0f);
        for (int n = 0; n < similarity.rows; ++n) {
            float* row = similarity.ptr<float>(n);
            const float shrinkage = memoryShrinkage.at<float>(0, n);
            for (int q = 0; q < similarity.cols; ++q)
                row[q] = (row[q] - bSq[q]) * shrinkage * invSqrtKeyDim;
        }

        const int topK = std::min(30, similarity.rows);
        for (int q = 0; q < similarity.cols; ++q) {
            std::vector<int> indices(similarity.rows);
            std::iota(indices.begin(), indices.end(), 0);
            std::partial_sort(indices.begin(), indices.begin() + topK, indices.end(),
                [&](int a, int b) { return similarity.at<float>(a, q) > similarity.at<float>(b, q); });

            float maxValue = similarity.at<float>(indices[0], q);
            for (int k = 1; k < topK; ++k)
                maxValue = std::max(maxValue, similarity.at<float>(indices[k], q));

            double sum = 0.0;
            std::vector<float> values(topK, 0.0f);
            for (int k = 0; k < topK; ++k) {
                values[k] = std::exp(similarity.at<float>(indices[k], q) - maxValue);
                sum += values[k];
            }
            if (sum <= 0.0)
                continue;
            for (int n = 0; n < similarity.rows; ++n)
                similarity.at<float>(n, q) = 0.0f;
            for (int k = 0; k < topK; ++k)
                similarity.at<float>(indices[k], q) = static_cast<float>(values[k] / sum);
        }

        cv::Mat readout;
        cv::gemm(memoryValue, similarity, 1.0, cv::Mat(), 0.0, readout);
        return MatFromReadout(readout);
    }

    void AddMemory(const cv::Mat& key, const cv::Mat& shrinkage, const cv::Mat& value)
    {
        MemoryFrame frame;
        frame.key = FlattenFeature(key, 64);
        frame.shrinkage = FlattenShrinkage(shrinkage);
        frame.value = FlattenFeature(value, 512);
        memoryFrames.push_back(frame);
        while (static_cast<int>(memoryFrames.size()) > maxMemoryFrames)
            memoryFrames.pop_front();
        RebuildMemory();
    }

    void EnsureHidden()
    {
        if (!hidden.empty())
            return;
        const int shape[] = {1, 1, 64, 30, 40};
        hidden = cv::Mat(5, shape, CV_32F, cv::Scalar(0.0f));
    }

public:
    void Load(const std::string& encodeKeyPath, const std::string& encodeValuePath, const std::string& decodePath)
    {
        encodeKey = cv::dnn::readNetFromONNX(encodeKeyPath);
        encodeValue = cv::dnn::readNetFromONNX(encodeValuePath);
        decode = cv::dnn::readNetFromONNX(decodePath);
    }

    void SetDevice(const std::string& processingDevice)
    {
        if (processingDevice == "GPU") {
            try {
                const std::vector<cv::dnn::Target> targets = cv::dnn::getAvailableTargets(cv::dnn::DNN_BACKEND_CUDA);
                if (std::find(targets.begin(), targets.end(), cv::dnn::DNN_TARGET_CUDA) != targets.end()) {
                    encodeKey.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
                    encodeKey.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
                    encodeValue.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
                    encodeValue.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
                    decode.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
                    decode.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
                    return;
                }
            } catch (const cv::Exception&) {
            }
        }

        encodeKey.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        encodeKey.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        encodeValue.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        encodeValue.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        decode.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        decode.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    }

    void Reset()
    {
        memoryFrames.clear();
        memoryKey.release();
        memoryShrinkage.release();
        memoryValue.release();
        hidden.release();
        frameIndex = 0;
        lastMemoryFrame = -1000000;
    }

    bool HasMemory() const
    {
        return !memoryFrames.empty();
    }

    cv::Mat Step(const cv::Mat& frame, const cv::Mat& seedMask = cv::Mat())
    {
        cv::Mat image = MakeXMemImageBlob(frame);

        encodeKey.setInput(image, "image");
        encodeKey.setInput(MakeScalarBlob(1.0f), "need_sk");
        encodeKey.setInput(MakeScalarBlob(1.0f), "need_ek");
        std::vector<cv::Mat> keyOutputs;
        encodeKey.forward(keyOutputs, std::vector<cv::String>{"key", "shrinkage", "selection", "f16", "f8", "f4"});
        cv::Mat key = keyOutputs[0];
        cv::Mat shrinkage = keyOutputs[1];
        cv::Mat f16 = keyOutputs[3];
        cv::Mat f8 = keyOutputs[4];
        cv::Mat f4 = keyOutputs[5];

        EnsureHidden();

        cv::Mat modelMask;
        if (!seedMask.empty()) {
            modelMask = MakeXMemMaskBlob(seedMask);
        } else if (HasMemory()) {
            cv::Mat memoryReadout = MatchMemory(key, keyOutputs[2]);
            decode.setInput(f16, "f16");
            decode.setInput(f8, "f8");
            decode.setInput(f4, "f4");
            decode.setInput(hidden, "h16_in");
            decode.setInput(memoryReadout, "memory_readout");
            decode.setInput(MakeScalarBlob(1.0f), "h_out");
            std::vector<cv::Mat> decodeOutputs;
            decode.forward(decodeOutputs, std::vector<cv::String>{"h16_out", "logits", "prob"});
            hidden = decodeOutputs[0].clone();
            modelMask = AggregateXMemForegroundProb(decodeOutputs[2]);
        } else {
            ++frameIndex;
            return cv::Mat();
        }

        const bool isMemoryFrame = !seedMask.empty() || frameIndex - lastMemoryFrame >= memEvery;
        if (isMemoryFrame) {
            const int othersShape[] = {1, 1, 480, 640};
            cv::Mat others(4, othersShape, CV_32F, cv::Scalar(0.0f));
            encodeValue.setInput(image, "image");
            encodeValue.setInput(f16, "f16");
            encodeValue.setInput(hidden, "h16_in");
            encodeValue.setInput(modelMask, "masks");
            encodeValue.setInput(others, "others");
            encodeValue.setInput(MakeScalarBlob(1.0f), "is_deep_update");
            std::vector<cv::Mat> valueOutputs;
            encodeValue.forward(valueOutputs, std::vector<cv::String>{"g16", "h16_out"});
            hidden = valueOutputs[1].clone();
            AddMemory(key, shrinkage, valueOutputs[0]);
            lastMemoryFrame = frameIndex;
        }

        cv::Mat outputMask = BinaryMaskFromXMemProb(modelMask);
        ++frameIndex;
        return outputMask;
    }
};

}

CVObjectMask::CVObjectMask(std::string processInfoJson, ProcessingController& controller)
    : processingController(&controller)
{
    SetJson(processInfoJson);
}

std::string CVObjectMask::ValidateONNXModel(std::string modelPath)
{
    return LoadONNXModel(modelPath, nullptr);
}

std::shared_ptr<Frame> CVObjectMask::PreviewSeedMask(std::shared_ptr<Frame> frame)
{
    if (!frame || efficientSamModelPath.empty() || promptKeyframes.empty())
        return std::shared_ptr<Frame>();

    std::string loadError = LoadONNXModel(efficientSamModelPath, &efficientSam);
    if (!loadError.empty())
        return std::shared_ptr<Frame>();
    SetProcessingDevice();

    CVObjectMaskPromptSet prompts = promptKeyframes.begin()->second;
    cv::Mat frameImage = frame->GetImageCV();
    cv::Mat seedMask = CreateEfficientSAMSeedMask(frameImage, prompts);
    if (seedMask.empty())
        return std::shared_ptr<Frame>();

    auto maskImage = std::make_shared<QImage>(
        seedMask.cols, seedMask.rows, QImage::Format_RGBA8888_Premultiplied);
    maskImage->fill(Qt::transparent);
    for (int y = 0; y < seedMask.rows; ++y) {
        const uint8_t* src = seedMask.ptr<uint8_t>(y);
        QRgb* dst = reinterpret_cast<QRgb*>(maskImage->scanLine(y));
        for (int x = 0; x < seedMask.cols; ++x)
            dst[x] = src[x] ? qRgba(255, 255, 255, 255) : qRgba(0, 0, 0, 0);
    }

    auto result = std::make_shared<Frame>(frame->number, seedMask.cols, seedMask.rows, "#000000");
    result->AddImage(maskImage);
    return result;
}

void CVObjectMask::SetProcessingDevice()
{
    if (processingDevice == "GPU") {
        try {
            const std::vector<cv::dnn::Target> targets = cv::dnn::getAvailableTargets(cv::dnn::DNN_BACKEND_CUDA);
            if (std::find(targets.begin(), targets.end(), cv::dnn::DNN_TARGET_CUDA) != targets.end()) {
                efficientSam.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
                efficientSam.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
                return;
            }
        } catch (const cv::Exception&) {
        }
        processingDevice = "CPU";
    }

    efficientSam.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    efficientSam.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
}

void CVObjectMask::maskClip(openshot::Clip& video, size_t _start, size_t _end, bool process_interval)
{
    start = _start;
    end = _end;

    video.Open();
    processingController->SetError(false, "");

    if (efficientSamModelPath.empty()) {
        processingController->SetError(true, "Missing path to EfficientSAM ONNX model file");
        error = true;
        return;
    }
    if (protobufDataPath.empty()) {
        processingController->SetError(true, "Missing path to object mask protobuf data file");
        error = true;
        return;
    }
    if (promptKeyframes.empty()) {
        processingController->SetError(true, "Missing positive prompt point for Object Mask preprocessing");
        error = true;
        return;
    }

    std::string loadError = LoadONNXModel(efficientSamModelPath, &efficientSam);
    if (!loadError.empty()) {
        processingController->SetError(true, loadError);
        error = true;
        return;
    }
    SetProcessingDevice();

    if (xmemEncodeKeyModelPath.empty() && !xmemModelDir.empty())
        xmemEncodeKeyModelPath = xmemModelDir + "/XMem-encode_key.onnx";
    if (xmemEncodeValueModelPath.empty() && !xmemModelDir.empty())
        xmemEncodeValueModelPath = xmemModelDir + "/XMem-encode_value-m1.onnx";
    if (xmemDecodeModelPath.empty() && !xmemModelDir.empty())
        xmemDecodeModelPath = xmemModelDir + "/XMem-decode-m1.onnx";
    if (xmemEncodeKeyModelPath.empty() || xmemEncodeValueModelPath.empty() || xmemDecodeModelPath.empty()) {
        processingController->SetError(true, "Missing path to XMem ONNX model files");
        error = true;
        return;
    }

    XMemPropagator xmem;
    try {
        xmem.Load(xmemEncodeKeyModelPath, xmemEncodeValueModelPath, xmemDecodeModelPath);
        xmem.SetDevice(processingDevice);
    } catch (const cv::Exception& e) {
        processingController->SetError(true, std::string("Failed to load XMem ONNX models: ") + e.what());
        error = true;
        return;
    } catch (const std::exception& e) {
        processingController->SetError(true, std::string("Failed to load XMem ONNX models: ") + e.what());
        error = true;
        return;
    }

    if (!process_interval || end <= 1 || end - start == 0) {
        start = static_cast<size_t>(video.Start() * video.Reader()->info.fps.ToFloat());
        end = static_cast<size_t>(video.End() * video.Reader()->info.fps.ToFloat());
    }
    if (end < start)
        end = start;

    CVObjectMaskPromptSet activePrompts;
    auto promptBeforeStart = promptKeyframes.upper_bound(start);
    if (promptBeforeStart != promptKeyframes.begin()) {
        --promptBeforeStart;
        activePrompts = promptBeforeStart->second;
    }
    auto firstPromptAtOrAfterStart = promptKeyframes.lower_bound(start);

    for (size_t frameNumber = start; frameNumber <= end; ++frameNumber) {
        if (processingController->ShouldStop())
            return;

        std::shared_ptr<openshot::Frame> frame = video.GetFrame(frameNumber);
        if (!frame)
            continue;

        auto promptIt = promptKeyframes.find(frameNumber);
        bool isPromptKeyframe = promptIt != promptKeyframes.end();
        if (promptIt != promptKeyframes.end()) {
            activePrompts = promptIt->second;
            xmem.Reset();
        } else if (!activePrompts.HasPositivePrompt()) {
            if (firstPromptAtOrAfterStart != promptKeyframes.end() && frameNumber >= firstPromptAtOrAfterStart->first) {
                activePrompts = firstPromptAtOrAfterStart->second;
                isPromptKeyframe = true;
                xmem.Reset();
            } else {
                CVObjectMaskFrameData emptyFrame;
                emptyFrame.frameId = frameNumber;
                masksData[frameNumber] = emptyFrame;
                continue;
            }
        }

        const cv::Mat frameImage = frame->GetImageCV();
        cv::Mat seedMask;
        if (isPromptKeyframe || !xmem.HasMemory()) {
            seedMask = CreateEfficientSAMSeedMask(frameImage, activePrompts);
            if (seedMask.empty()) {
                CVObjectMaskFrameData emptyFrame;
                emptyFrame.frameId = frameNumber;
                masksData[frameNumber] = emptyFrame;
                continue;
            }
            if (!isPromptKeyframe)
                xmem.Reset();
        }

        cv::Mat propagatedMask;
        try {
            propagatedMask = xmem.Step(frameImage, seedMask);
        } catch (const cv::Exception& e) {
            processingController->SetError(true, std::string("Failed to propagate Object Mask with XMem: ") + e.what());
            error = true;
            return;
        }

        cv::Mat outputMask;
        if (!seedMask.empty()) {
            outputMask = seedMask;
        } else if (!propagatedMask.empty()) {
            cv::resize(propagatedMask, outputMask, frameImage.size(), 0, 0, cv::INTER_NEAREST);
        }
        masksData[frameNumber] = FrameDataFromMask(outputMask, frameNumber, 1.0f);

        const size_t range = std::max<size_t>(1, end - start);
        processingController->SetProgress(uint(100 * (frameNumber - start) / range));
    }
}

cv::Mat CVObjectMask::CreateEfficientSAMSeedMask(const cv::Mat& frame, const CVObjectMaskPromptSet& prompts)
{
    EfficientSamPreprocessResult prep = MakeEfficientSamBlob(frame, modelSize);
    std::vector<cv::Point> backgroundPoints;
    cv::Mat pointCoords = MakeEfficientSamPromptBlob(prompts, prep, promptSlots, backgroundPoints);
    cv::Mat pointLabels = MakeEfficientSamLabelBlob(prompts, promptSlots);

    efficientSam.setInput(prep.blob, "batched_images");
    efficientSam.setInput(pointCoords, "batched_point_coords");
    efficientSam.setInput(pointLabels, "batched_point_labels");

    std::vector<cv::Mat> outputs;
    efficientSam.forward(outputs, std::vector<cv::String>{"output_masks", "iou_predictions"});
    if (outputs.size() != 2)
        return cv::Mat();

    cv::Mat modelMask = SelectEfficientSamMask(outputs[0], outputs[1], backgroundPoints, maskThreshold);
    if (modelMask.empty())
        return cv::Mat();
    return EfficientSamMaskToFrameMask(modelMask, frame.size(), maskThreshold);
}

bool CVObjectMask::SaveObjMaskData()
{
    if (protobufDataPath.empty()) {
        std::cerr << "Missing path to object mask protobuf data file." << std::endl;
        return false;
    }
    if (error)
        return false;

    pb_objdetect::ObjDetect objMessage;
    objMessage.add_classnames()->assign("object mask");

    for (const auto& frameData : masksData)
        AddFrameDataToProto(objMessage.add_frame(), frameData.second);

    *objMessage.mutable_last_updated() = TimeUtil::SecondsToTimestamp(time(NULL));

    std::fstream output(protobufDataPath, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!objMessage.SerializeToOstream(&output)) {
        std::cerr << "Failed to write object mask protobuf message." << std::endl;
        return false;
    }

    return true;
}

void CVObjectMask::AddFrameDataToProto(pb_objdetect::Frame* pbFrameData, const CVObjectMaskFrameData& frameData)
{
    pbFrameData->set_id(frameData.frameId);
    if (!frameData.HasMask())
        return;

    pb_objdetect::Frame_Box* box = pbFrameData->add_bounding_box();
    box->set_x(frameData.box.x);
    box->set_y(frameData.box.y);
    box->set_w(frameData.box.width);
    box->set_h(frameData.box.height);
    box->set_classid(0);
    box->set_confidence(frameData.score);
    box->set_objectid(frameData.objectId);

    pb_objdetect::Frame_Box_Mask* mask = box->mutable_mask();
    mask->set_width(frameData.width);
    mask->set_height(frameData.height);
    for (uint32_t count : frameData.rle)
        mask->add_rle(count);
}

void CVObjectMask::SetJson(const std::string value)
{
    try {
        SetJsonValue(openshot::stringToJson(value));
    } catch (const std::exception&) {
        std::cout << "JSON is invalid (missing keys or invalid data types)" << std::endl;
    }
}

void CVObjectMask::SetJsonValue(const Json::Value root)
{
    if (!root["protobuf_data_path"].isNull())
        protobufDataPath = root["protobuf_data_path"].asString();
    if (!root["efficient_sam_model"].isNull())
        efficientSamModelPath = root["efficient_sam_model"].asString();
    if (!root["efficient_sam_model_path"].isNull())
        efficientSamModelPath = root["efficient_sam_model_path"].asString();
    if (!root["sam_model"].isNull())
        efficientSamModelPath = root["sam_model"].asString();
    if (!root["sam_model_path"].isNull())
        efficientSamModelPath = root["sam_model_path"].asString();
    if (!root["encoder_model"].isNull())
        efficientSamModelPath = root["encoder_model"].asString();
    if (!root["encoder_model_path"].isNull())
        efficientSamModelPath = root["encoder_model_path"].asString();
    if (!root["xmem_model_dir"].isNull())
        xmemModelDir = root["xmem_model_dir"].asString();
    if (!root["xmem_encode_key_model"].isNull())
        xmemEncodeKeyModelPath = root["xmem_encode_key_model"].asString();
    if (!root["xmem_encode_key_model_path"].isNull())
        xmemEncodeKeyModelPath = root["xmem_encode_key_model_path"].asString();
    if (!root["xmem_encode_value_model"].isNull())
        xmemEncodeValueModelPath = root["xmem_encode_value_model"].asString();
    if (!root["xmem_encode_value_model_path"].isNull())
        xmemEncodeValueModelPath = root["xmem_encode_value_model_path"].asString();
    if (!root["xmem_decode_model"].isNull())
        xmemDecodeModelPath = root["xmem_decode_model"].asString();
    if (!root["xmem_decode_model_path"].isNull())
        xmemDecodeModelPath = root["xmem_decode_model_path"].asString();
    if (!root["processing-device"].isNull())
        processingDevice = root["processing-device"].asString();
    if (!root["processing_device"].isNull())
        processingDevice = root["processing_device"].asString();
    if (!root["prompt_slots"].isNull())
        promptSlots = std::max(1, std::min(6, root["prompt_slots"].asInt()));
    if (!root["mask_threshold"].isNull())
        maskThreshold = root["mask_threshold"].asFloat();
    if (!root["model_size"].isNull())
        modelSize = root["model_size"].asInt();
    promptKeyframes.clear();
    if (!root["object_mask_selection"].isNull()) {
        const Json::Value& selection = root["object_mask_selection"];
        const Json::Value& frames = selection["frames"];
        if (frames.isObject()) {
            for (const auto& frameName : frames.getMemberNames()) {
                const size_t frameNumber = JsonFrameNumber(frameName);
                if (frameNumber == 0)
                    continue;
                CVObjectMaskPromptSet prompts = PromptSetFromJson(frames[frameName]);
                if (prompts.HasPositivePrompt())
                    promptKeyframes[frameNumber] = prompts;
            }
        }
    }

    CVObjectMaskPromptSet legacyPrompts;
    if (!root["positive_points"].isNull())
        AppendJsonPoints(root["positive_points"], legacyPrompts.positivePoints);
    if (!root["negative_points"].isNull())
        AppendJsonPoints(root["negative_points"], legacyPrompts.negativePoints);

    if (!root["positive_x"].isNull() && !root["positive_y"].isNull()) {
        cv::Point2f point(root["positive_x"].asFloat(), root["positive_y"].asFloat());
        if (IsValidPoint(point) && legacyPrompts.positivePoints.empty())
            legacyPrompts.positivePoints.push_back(point);
    }
    if (!root["negative_x"].isNull() && !root["negative_y"].isNull()) {
        cv::Point2f point(root["negative_x"].asFloat(), root["negative_y"].asFloat());
        if (IsValidPoint(point) && legacyPrompts.negativePoints.empty())
            legacyPrompts.negativePoints.push_back(point);
    }
    if (!root["rect_x1"].isNull() && !root["rect_y1"].isNull() &&
        !root["rect_x2"].isNull() && !root["rect_y2"].isNull()) {
        Json::Value rect;
        rect["x1"] = root["rect_x1"];
        rect["y1"] = root["rect_y1"];
        rect["x2"] = root["rect_x2"];
        rect["y2"] = root["rect_y2"];
        ApplyRectJson(rect, legacyPrompts);
    }
    if (legacyPrompts.HasPositivePrompt() && promptKeyframes.empty())
        promptKeyframes[1] = legacyPrompts;
}

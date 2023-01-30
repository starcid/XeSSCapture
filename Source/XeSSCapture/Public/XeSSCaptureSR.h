#pragma once

class XeSSCaptureSR
{
public:
	static void Capture();

protected:
	static void SaveSampleToJson(const FString& fileName, float sampleX, float sampleY);
	static void CaptureSample();
};
#pragma once

#include <SimpleMath.h>
using namespace DirectX::SimpleMath;

namespace tinygltf
{
	class Model;
	class Image;
	class Sampler;
}

class FController;

struct FRenderMesh
{
	std::string m_name;
	size_t m_indexCount;
	uint32_t m_indexOffset;
	uint32_t m_positionOffset;
	uint32_t m_normalOffset;
	uint32_t m_uvOffset;

	std::string m_materialName;
	Vector3 m_emissiveFactor;
	Vector3 m_baseColorFactor;
	float m_metallicFactor;
	float m_roughnessFactor;
	int m_baseColorTextureIndex;
	int m_metallicRoughnessTextureIndex;
	int m_normalTextureIndex;
	int m_baseColorSamplerIndex;
	int m_metallicRoughnessSamplerIndex;
	int m_normalSamplerIndex;
};

struct FCamera
{
	std::string m_name;
	Matrix m_viewTransform;
	Matrix m_projectionTransform;
};

struct FLightProbe
{
	int m_envmapTextureIndex;
	int m_shTextureIndex;
	int m_prefilteredEnvmapTextureIndex;
};

struct FScene
{
	void Reload(const std::string& filename);
	void LoadNode(int nodeIndex, const tinygltf::Model& model, const Matrix& transform);
	void LoadMesh(int meshIndex, const tinygltf::Model& model, const Matrix& transform);
	void LoadCamera(int meshIndex, const tinygltf::Model& model, const Matrix& transform);
	void Clear();

	// Scene file
	std::string m_sceneFilename = {};

	// Scene entity lists
	std::vector<FRenderMesh> m_meshGeo;
	std::vector<Matrix> m_meshTransforms;
	std::vector<DirectX::BoundingBox> m_meshBounds; // object space
	std::vector<FCamera> m_cameras;

	// Scene geo
	std::unique_ptr<FBindlessShaderResource> m_meshIndexBuffer;
	std::unique_ptr<FBindlessShaderResource> m_meshPositionBuffer;
	std::unique_ptr<FBindlessShaderResource> m_meshNormalBuffer;
	std::unique_ptr<FBindlessShaderResource> m_meshUvBuffer;
	DirectX::BoundingBox m_sceneBounds; // world space

	// Image based lighting
	FLightProbe m_globalLightProbe;

	// Transform
	Matrix m_rootTransform;

private:
	int LoadTexture(const tinygltf::Image& image, const bool srgb);
	int LoadSampler(const tinygltf::Sampler& sampler);

private:
	uint8_t* m_scratchIndexBuffer;
	uint8_t* m_scratchPositionBuffer;
	uint8_t* m_scratchNormalBuffer;
	uint8_t* m_scratchUvBuffer;

	size_t m_scratchIndexBufferOffset;
	size_t m_scratchPositionBufferOffset;
	size_t m_scratchNormalBufferOffset;
	size_t m_scratchUvBufferOffset;
};

struct FView
{
	void Tick(const float deltaTime, const FController* controller);
	void Reset(const FScene& scene);

	Vector3 m_position;
	Vector3 m_right;
	Vector3 m_up;
	Vector3 m_look;

	Matrix m_viewTransform;
	Matrix m_projectionTransform;

private:
	void UpdateViewTransform();
};

namespace Demo
{
	const FScene* GetScene();
	const FView* GetView();
}
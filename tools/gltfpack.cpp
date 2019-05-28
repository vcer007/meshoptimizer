#include "../src/meshoptimizer.h"

#include <string>
#include <vector>

#include <math.h>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

struct Attr
{
	float f[4];
};

struct Stream
{
	cgltf_attribute_type type;
	int index;

	std::vector<Attr> data;
};

struct Mesh
{
	std::vector<Stream> streams;
	std::vector<unsigned int> indices;
};

struct Scene
{
	cgltf_data* data;
	std::vector<Mesh> meshes;

	~Scene()
	{
		cgltf_free(data);
	}
};

const char* getError(cgltf_result result)
{
	switch (result)
	{
	case cgltf_result_file_not_found:
		return "file not found";

	case cgltf_result_io_error:
		return "I/O error";

	case cgltf_result_invalid_json:
		return "invalid JSON";

	case cgltf_result_invalid_gltf:
		return "invalid GLTF";

	case cgltf_result_out_of_memory:
		return "out of memory";

	default:
		return "unknown error";
	}
}

cgltf_accessor* getAccessor(const cgltf_attribute* attributes, size_t attribute_count, cgltf_attribute_type type, int index = 0)
{
	for (size_t i = 0; i < attribute_count; ++i)
		if (attributes[i].type == type && attributes[i].index == index)
			return attributes[i].data;

	return 0;
}

void transformPosition(float* ptr, const float* transform)
{
	float x = ptr[0] * transform[0] + ptr[1] * transform[4] + ptr[2] * transform[8] + transform[12];
	float y = ptr[0] * transform[1] + ptr[1] * transform[5] + ptr[2] * transform[9] + transform[13];
	float z = ptr[0] * transform[2] + ptr[1] * transform[6] + ptr[2] * transform[10] + transform[14];

	ptr[0] = x;
	ptr[1] = y;
	ptr[2] = z;
}

void transformNormal(float* ptr, const float* transform)
{
	float x = ptr[0] * transform[0] + ptr[1] * transform[4] + ptr[2] * transform[8];
	float y = ptr[0] * transform[1] + ptr[1] * transform[5] + ptr[2] * transform[9];
	float z = ptr[0] * transform[2] + ptr[1] * transform[6] + ptr[2] * transform[10];

	float l = sqrtf(x * x + y * y + z * z);
	float s = (l == 0.f) ? 0.f : 1 / l;

	ptr[0] = x * s;
	ptr[1] = y * s;
	ptr[2] = z * s;
}

void parseMeshes(cgltf_data* data, std::vector<Mesh>& meshes)
{
	for (size_t ni = 0; ni < data->nodes_count; ++ni)
	{
		if (!data->nodes[ni].mesh)
			continue;

		float transform[16];
		cgltf_node_transform_world(&data->nodes[ni], transform);

		const cgltf_mesh& mesh = *data->nodes[ni].mesh;

		for (size_t pi = 0; pi < mesh.primitives_count; ++pi)
		{
			const cgltf_primitive& primitive = mesh.primitives[pi];

			Mesh result;

			if (cgltf_accessor* a = primitive.indices)
			{
				result.indices.resize(a->count);
				for (size_t i = 0; i < a->count; ++i)
					result.indices[i] = cgltf_accessor_read_index(a, i);
			}

			for (size_t ai = 0; ai < primitive.attributes_count; ++ai)
			{
				const cgltf_attribute& attr = primitive.attributes[ai];

				if (attr.type == cgltf_attribute_type_invalid)
					continue;

				Stream s = { attr.type, attr.index };
				s.data.resize(attr.data->count);

				for (size_t i = 0; i < attr.data->count; ++i)
					cgltf_accessor_read_float(attr.data, i, s.data[i].f, 4);

				if (attr.type == cgltf_attribute_type_position)
				{
					for (size_t i = 0; i < attr.data->count; ++i)
						transformPosition(s.data[i].f, transform);
				}
				else if (attr.type == cgltf_attribute_type_normal || attr.type == cgltf_attribute_type_tangent)
				{
					for (size_t i = 0; i < attr.data->count; ++i)
						transformNormal(s.data[i].f, transform);
				}

				result.streams.push_back(s);
			}

			if (result.indices.size() && result.streams.size())
				meshes.push_back(result);
		}
	}
}

bool process(Scene& scene, std::string& json, std::string& bin)
{
	cgltf_data* data = scene.data;

	parseMeshes(data, scene.meshes);

	(void)json;
	(void)bin;
	return true;
}

void writeU32(FILE* out, uint32_t data)
{
	fwrite(&data, 4, 1, out);
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "Usage: gltfpack [options] input output\n");
		return 1;
	}

	const char* input = argv[argc - 2];
	const char* output = argv[argc - 1];

	Scene scene = {};

	cgltf_options options = {};
	cgltf_result result = cgltf_parse_file(&options, input, &scene.data);
	result = (result == cgltf_result_success) ? cgltf_validate(scene.data) : result;
	result = (result == cgltf_result_success) ? cgltf_load_buffers(&options, scene.data, input) : result;

	if (result != cgltf_result_success)
	{
		fprintf(stderr, "Error loading %s: %s\n", input, getError(result));
		return 2;
	}

	std::string json, bin;
	if (!process(scene, json, bin))
	{
		fprintf(stderr, "Error processing %s\n", input);
		return 3;
	}

	const char* ext = strrchr(output, '.');

	if (ext && (strcmp(ext, ".gltf") == 0 || strcmp(ext, ".GLTF") == 0))
	{
		std::string binpath = output;
		binpath.replace(binpath.size() - 5, 5, ".bin");

		std::string binname = binpath;
		std::string::size_type slash = binname.find_last_of("/\\");
		if (slash != std::string::npos)
			binname.erase(0, slash);

		FILE* outjson = fopen(output, "wb");
		FILE* outbin = fopen(binpath.c_str(), "wb");
		if (!outjson || !outbin)
		{
			fprintf(stderr, "Error saving %s\n", output);
			return 4;
		}

		fprintf(outjson, "{buffers:[{uri:\"%s\",byteLength:%zu}],", binname.c_str(), bin.size());
		fwrite(json.c_str(), json.size(), 1, outjson);
		fprintf(outjson, "}");

		fwrite(bin.c_str(), bin.size(), 1, outbin);

		fclose(outjson);
		fclose(outbin);
	}
	else if (ext && (strcmp(ext, ".glb") == 0 || strcmp(ext, ".GLB") == 0))
	{
		FILE* out = fopen(output, "wb");
		if (!out)
		{
			fprintf(stderr, "Error saving %s\n", output);
			return 4;
		}

		char bufferspec[32];
		sprintf(bufferspec, "{buffers:[{byteLength:%zu}],", bin.size());

		json.insert(0, bufferspec);
		json.push_back('}');

		while (json.size() % 4)
			json.push_back(' ');

		while (bin.size() % 4)
			bin.push_back('\0');

		writeU32(out, 0x46546C67);
		writeU32(out, 2);
		writeU32(out, 12 + 8 + json.size() + 8 + bin.size());

		writeU32(out, 0x4E4F534A);
		writeU32(out, json.size());
		fwrite(json.c_str(), json.size(), 1, out);

		writeU32(out, 0x004E4942);
		writeU32(out, bin.size());
		fwrite(bin.c_str(), bin.size(), 1, out);

		fclose(out);
	}
	else
	{
		fprintf(stderr, "Error saving %s: unknown extension (expected .gltf or .glb)\n", output);
		return 4;
	}

	return 0;
}
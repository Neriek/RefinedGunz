#include "stdafx.h"
#include "MeshManager.h"
#include "rapidxml.hpp"
#include <fstream>
#include "defer.h"

TaskManager::TaskManager()
{
	thr = std::thread([this](){ while (true) ThreadLoop(); });
	thr.detach();
}

void TaskManager::Update(float Elapsed)
{
	std::unique_lock<std::mutex> lock(QueueMutex[1]);

	while (Invokations.size())
	{
		Invokations.front()();
		Invokations.pop();
	}
}

void TaskManager::ThreadLoop()
{
	std::unique_lock<std::mutex> lock(QueueMutex[0]);
	cv.wait(lock, [this]() { return Notified; });

	while (Tasks.size())
	{
		Tasks.front()();
		Tasks.pop();
	}

	Notified = false;
}

#ifdef _DEBUG
#define LOG(...) DMLog(__VA_ARGS__)
#else
static void nop(...) {}
// The arguments are passed to a function that does nothing so that they are still evaluated.
// If the macro were empty, it would hide compilation errors in Release mode.
#define LOG(...) nop(__VA_ARGS__)
#endif

bool MeshManager::LoadParts(std::vector<unsigned char>& File)
{
	// TODO: Parse elu files that aren't in parts_index.xml

	rapidxml::xml_document<> parts;

	try
	{
		parts.parse<rapidxml::parse_non_destructive>(reinterpret_cast<char*>(&File[0]), File.size());
	}
	catch (rapidxml::parse_error &e)
	{
		MLog("RapidXML threw parse_error (%s) on parts_index.xml at %s\n", e.what(), e.where<char>());
		return false;
	}

#define LOOP_NODES(var_name, parent, tag) \
for (auto var_name = (parent)->first_node(tag); var_name; var_name = var_name->next_sibling(tag))
#define LOOP_ATTRIBUTES(var_name, parent, tag) \
for (auto var_name = (parent)->first_attribute(tag); var_name; var_name = var_name->next_attribute(tag))

	LOOP_NODES(listnode, &parts, "partslisting")
	{
		auto meshattr = listnode->first_attribute("mesh");
		if (!meshattr || !meshattr->value())
			continue;

		// The strings aren't zero-terminated in RapidXML's non-destructive mode,
		// so we need to carefully construct an std::string with the size in mind.
		auto AttributeToString = [&](auto* attr) {
			return std::string{ attr->value(), attr->value_size() };
		};

		auto MeshName = AttributeToString(meshattr);

		auto MeshEmplaceResult = BaseMeshMap.emplace(MeshName, BaseMeshData{});
		auto&& PartsToEluMap = MeshEmplaceResult.first->second.PartsToEluMap;

		// parts tag example:
		// <parts file="Model/woman/woman-parts11.elu" part="eq_chest_05" part="eq_legs_005"
		//     part="eq_feet_005" part="eq_hands_05" part="eq_head_02" part="eq_face_004"/>
		LOOP_NODES(node, listnode, "parts")
		{
			auto fileattr = node->first_attribute("file");
			if (!fileattr || !fileattr->value())
				continue;

			LOOP_ATTRIBUTES(attr, node, "part")
			{
				if (!attr->value())
					continue;

				auto PartsName = AttributeToString(attr);
				auto EluPath = AttributeToString(fileattr);
				PartsToEluMap.emplace(PartsName, EluPath);

				LOG("Added %s -> %s\n", PartsName.c_str(), EluPath.c_str());
			}
		}
	}

#undef LOOP_NODES
#undef LOOP_ATTRIBUTES

	return true;
}

void MeshManager::Destroy()
{
	LOG("MeshManager::Destroy()\n");

	for (auto&& Pair : BaseMeshMap)
	{
		auto&& BaseMeshName = Pair.first;
		auto&& BaseMesh = Pair.second;
		LOG("BaseMesh %s\n"
			"PartsToEluMap.size() = %zu\n"
			"AllocatedMeshes.size() = %zu\n"
			"AllocatedNodes.size() = %zu\n"
			"\n",
			BaseMeshName,
			BaseMesh.PartsToEluMap.size(),
			BaseMesh.AllocatedMeshes.size(),
			BaseMesh.AllocatedNodes.size());
	}

	BaseMeshMap.clear();
}

RMeshNode *MeshManager::Get(const char *szMeshName, const char *szNodeName)
{
	auto Prof = MBeginProfile("MeshManager::Get");

	std::unique_lock<std::mutex> lock{ mutex };

	LOG("Get mesh: %s, node: %s\n", szMeshName, szNodeName);

	auto mesh_it = BaseMeshMap.find(szMeshName);
	if (mesh_it == BaseMeshMap.end())
	{
		LOG("Couldn't find mesh %s in MeshMap!\n", szMeshName);
		return nullptr;
	}

	auto&& BaseMesh = mesh_it->second;

	auto alloc_node_it = BaseMesh.AllocatedNodes.find(szNodeName);

	if (alloc_node_it != BaseMesh.AllocatedNodes.end())
	{
		auto* meshnode = alloc_node_it->second.Obj;

		LOG("Found already allocated node %s %p\n",
			meshnode->GetName(), static_cast<void*>(meshnode));
		
		auto* mesh = meshnode->m_pParentMesh;
		auto alloc_mesh_it = BaseMesh.AllocatedMeshes.find(mesh->GetFileName());
		if (alloc_mesh_it == BaseMesh.AllocatedMeshes.end())
		{
			LOG("Couldn't find mesh %s %p in AllocatedMeshes\n",
				mesh->GetFileName(), static_cast<void*>(mesh));
			return nullptr;
		}

		LOG("Returning already allocated node %s %p in mesh %s %p\n",
			meshnode->GetName(), static_cast<void*>(meshnode),
			mesh->GetFileName(), static_cast<void*>(mesh));

		alloc_mesh_it->second.References++;
		alloc_node_it->second.References++;
		return meshnode;
	}

	auto nodeit = BaseMesh.PartsToEluMap.find(szNodeName);

	if (nodeit == BaseMesh.PartsToEluMap.end())
	{ 
		LOG("Couldn't find node %s in PartsToEluMap element\n", szNodeName);

		return nullptr;
	}

	RMesh *pMesh = nullptr;

	auto allocmesh = BaseMesh.AllocatedMeshes.find(nodeit->second);

	if (allocmesh != BaseMesh.AllocatedMeshes.end())
	{
		pMesh = allocmesh->second.Obj.get();
		allocmesh->second.References++;
		LOG("Found already allocated mesh %s %p, new ref count %d\n",
			pMesh->GetFileName(), static_cast<void*>(pMesh),
			allocmesh->second.References);
	}
	else
	{
		auto UniqueMeshPtr = RMeshPtr{ new RMesh };
		pMesh = UniqueMeshPtr.get();

		// Unlock the mutex since ReadElu takes a while
		lock.unlock();
		if (!pMesh->ReadElu(nodeit->second.c_str()))
		{
			MLog("Couldn't load elu %s\n", nodeit->second.c_str());
			return nullptr;
		}
		lock.lock();

		LOG("Loaded mesh %s, %s, %p\n",
			nodeit->second.c_str(), pMesh->GetFileName(),
			static_cast<void*>(pMesh));

		BaseMesh.AllocatedMeshes.emplace(nodeit->second.c_str(),
			Allocation<RMeshPtr>{ std::move(UniqueMeshPtr), 1 });
	}

	auto node = pMesh->GetMeshData(szNodeName);

	if (!node)
	{
		LOG("Failed to find node %s\n", szNodeName);

		return nullptr;
	}

	LOG("Placing %p -> %p in MeshNodeToMeshMap\n",
		static_cast<void*>(node), static_cast<void*>(pMesh));

	BaseMesh.AllocatedNodes.emplace(szNodeName, Allocation<RMeshNode*>{ node, 1 });

	return node;
}

void MeshManager::Release(RMeshNode *pNode)
{
	auto Prof = MBeginProfile("MeshManager::Release");

	std::lock_guard<std::mutex> lock(mutex);

	LOG("Release %s %p, parts type %d\n",
		pNode->GetName(), static_cast<void*>(pNode),
		pNode->m_PartsType);

	switch (pNode->m_PartsType)
	{
	case eq_parts_head:
	case eq_parts_face:
	case eq_parts_chest:
	case eq_parts_hands:
	case eq_parts_legs:
	case eq_parts_feet:
		break;
	default:
		LOG("Unsuitable parts type %d, returning\n",
			pNode->m_PartsType);
		return;
	};

	auto* NodeBaseMesh = pNode->m_pBaseMesh;
	auto* BaseMeshName = NodeBaseMesh->GetName();
	LOG("Base mesh %s\n", BaseMeshName);

	auto&& BaseMeshIt = BaseMeshMap.find(BaseMeshName);
	if (BaseMeshIt == BaseMeshMap.end())
	{
		LOG("Failed to find base mesh %s in base mesh map!\n", BaseMeshName);
		return;
	}

	auto&& BaseMesh = BaseMeshIt->second;

	auto GetMeshAndReleaseNodeAllocation = [&]() -> RMesh*
	{
		auto AllocIt = BaseMesh.AllocatedNodes.find(pNode->GetName());
		if (AllocIt == BaseMesh.AllocatedNodes.end())
		{
			LOG("Couldn't find allocated node %s\n", pNode->GetName());
			return nullptr;
		}
		
		auto* pMesh = AllocIt->second.Obj->m_pParentMesh;
		DecrementRefCount(BaseMesh.AllocatedNodes, AllocIt);

		return pMesh;
	};
	auto* pMesh = GetMeshAndReleaseNodeAllocation();
	if (!pMesh)
		return;

	LOG("Filename %s, %d\n", pMesh->m_FileName.c_str(), pMesh->m_FileName.length());

	auto allocmesh = BaseMesh.AllocatedMeshes.find(pMesh->GetFileName());
	if (allocmesh == BaseMesh.AllocatedMeshes.end())
	{
		LOG("Couldn't find mesh %s to release\n", pMesh->GetFileName());
		return;
	}

	DecrementRefCount(BaseMesh.AllocatedMeshes, allocmesh);
}

void MeshManager::DecrementRefCount(AllocatedMeshesType& AllocatedMeshes, AllocatedMeshesType::iterator AllocIt)
{
	auto& alloc = AllocIt->second;
	auto* mesh = alloc.Obj.get();
	alloc.References--;

	LOG("DecRefCount mesh %s %p, new ref count %d\n",
		mesh->GetFileName(), static_cast<void*>(mesh),
		alloc.References);

	if (alloc.References <= 0)
	{
		LOG("Releasing mesh %s %p\n",
			mesh->GetFileName(), static_cast<void*>(mesh));

		AllocatedMeshes.erase(AllocIt);
	}
}

void MeshManager::DecrementRefCount(AllocatedNodesType& AllocatedNodes, AllocatedNodesType::iterator AllocIt)
{
	auto& alloc = AllocIt->second;
	auto* meshnode = alloc.Obj;
	alloc.References--;

	LOG("DecRefCount node %s %p, new ref count %d\n",
		meshnode->GetName(), static_cast<void*>(meshnode),
		alloc.References);

	if (alloc.References <= 0)
	{
		LOG("Erasing node allocation %s %p\n",
			meshnode->GetName(), static_cast<void*>(meshnode));

		AllocatedNodes.erase(AllocIt);
	}
}

bool MeshManager::RemoveObject(void *Obj, bool All)
{
	std::lock_guard<std::mutex> lock(ObjQueueMutex);

	if (All)
	{
		auto it = std::remove(QueuedObjs.begin(), QueuedObjs.end(), Obj);

		if (it == QueuedObjs.end())
			return false;

		QueuedObjs.erase(it, QueuedObjs.end());
	}
	else
	{
		auto it = std::find(QueuedObjs.begin(), QueuedObjs.end(), Obj);

		if (it == QueuedObjs.end())
			return false;

		QueuedObjs.erase(it);
	}

	return true;
}

void MeshManager::GetAsync(const char *szMeshName, const char *szNodeName, void* Obj,
	std::function<void(RMeshNode*)> Callback)
{
	{
		std::lock_guard<std::mutex> lock(ObjQueueMutex);

		QueuedObjs.push_back(Obj);
	}

	auto Task = [this,
		PersistentMesh = std::string{ szMeshName }, PersistentNode = std::string{ szNodeName },
		Obj, Callback]
		()
	{
		auto ret = Get(PersistentMesh.c_str(), PersistentNode.c_str());

		TaskManager::GetInstance().Invoke([=]() {
			// We want to make sure the object hasn't been destroyed
			// between the GetAsync call and the invokation.
			if (RemoveObject(Obj, false))
				Callback(ret);
		});
	};
	TaskManager::GetInstance().AddTask(std::move(Task));
}

MeshManager* GetMeshManager()
{
	static MeshManager Instance;
	return &Instance;
}

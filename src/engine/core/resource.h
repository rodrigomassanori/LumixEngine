#pragma once


#include "core/fs/ifile_system_defines.h"
#include "core/delegate_list.h"
#include "core/path.h"


namespace Lumix
{


class ResourceManager;


class LUMIX_ENGINE_API Resource
{
public:
	friend class ResourceManagerBase;

	enum class State : uint32_t
	{
		EMPTY = 0,
		READY,
		FAILURE,
	};

	typedef DelegateList<void(State, State)> ObserverCallback;

public:
	State getState() const { return m_current_state; }

	bool isEmpty() const { return State::EMPTY == m_current_state; }
	bool isReady() const { return State::READY == m_current_state; }
	bool isFailure() const { return State::FAILURE == m_current_state; }
	uint32_t getRefCount() const { return m_ref_count; }
	ObserverCallback& getObserverCb() { return m_cb; }
	size_t size() const { return m_size; }
	const Path& getPath() const { return m_path; }
	ResourceManager& getResourceManager() { return m_resource_manager; }

	template <typename C, void (C::*Function)(State, State)> void onLoaded(C* instance)
	{
		m_cb.bind<C, Function>(instance);
		if (isReady())
		{
			(instance->*Function)(State::READY, State::READY);
		}
	}

protected:
	Resource(const Path& path, ResourceManager& resource_manager, IAllocator& allocator);
	virtual ~Resource();

	virtual void onBeforeReady() {}
	virtual void unload(void) = 0;
	virtual bool load(FS::IFile& file) = 0;

	void onCreated(State state);
	void doUnload();

	void addDependency(Resource& dependent_resource);
	void removeDependency(Resource& dependent_resource);

protected:
	size_t m_size;
	ResourceManager& m_resource_manager;

private:
	void doLoad();
	void fileLoaded(FS::IFile& file, bool success, FS::FileSystem& fs);
	void onStateChanged(State old_state, State new_state);
	void checkState();
	uint32_t addRef(void) { return ++m_ref_count; }
	uint32_t remRef(void) { return --m_ref_count; }

	Resource(const Resource&);
	void operator=(const Resource&);

private:
	ObserverCallback m_cb;
	Path m_path;
	uint16_t m_ref_count;
	uint16_t m_empty_dep_count;
	uint16_t m_failed_dep_count;
	State m_current_state;
	State m_desired_state;
}; // class Resource


} // ~namespace Lumix

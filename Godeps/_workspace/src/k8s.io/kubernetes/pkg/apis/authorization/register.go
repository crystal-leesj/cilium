/*
Copyright 2015 The Kubernetes Authors All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

package authorization

import (
	"github.com/noironetworks/cilium-net/Godeps/_workspace/src/k8s.io/kubernetes/pkg/api/unversioned"
	"github.com/noironetworks/cilium-net/Godeps/_workspace/src/k8s.io/kubernetes/pkg/runtime"
)

// GroupName is the group name use in this package
const GroupName = "authorization.k8s.io"

// SchemeGroupVersion is group version used to register these objects
var SchemeGroupVersion = unversioned.GroupVersion{Group: GroupName, Version: runtime.APIVersionInternal}

// Kind takes an unqualified kind and returns back a Group qualified GroupKind
func Kind(kind string) unversioned.GroupKind {
	return SchemeGroupVersion.WithKind(kind).GroupKind()
}

// Resource takes an unqualified resource and returns back a Group qualified GroupResource
func Resource(resource string) unversioned.GroupResource {
	return SchemeGroupVersion.WithResource(resource).GroupResource()
}

func AddToScheme(scheme *runtime.Scheme) {
	addKnownTypes(scheme)
}

func addKnownTypes(scheme *runtime.Scheme) {
	scheme.AddKnownTypes(SchemeGroupVersion,
		&SelfSubjectAccessReview{},
		&SubjectAccessReview{},
		&LocalSubjectAccessReview{},
	)
}

func (obj *LocalSubjectAccessReview) GetObjectKind() unversioned.ObjectKind { return &obj.TypeMeta }
func (obj *SubjectAccessReview) GetObjectKind() unversioned.ObjectKind      { return &obj.TypeMeta }
func (obj *SelfSubjectAccessReview) GetObjectKind() unversioned.ObjectKind  { return &obj.TypeMeta }
